#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include "IotWebConf.h"
#include <ArduinoJson.h>
#include "AsyncMqttClient.h"
#include <esp32ModbusTCP.h>
#include "Log.h"
#include "ChargeControllerInfo.h"

#define MODBUS_POLL_RATE 5000
#define WAKE_PUBLISH_RATE 5000
#define SNOOZE_PUBLISH_RATE 300000
#define WAKE_COUNT 60
#define CONFIG_VERSION "V1.2.3"
#define PORT_CONFIG_LEN 6

AsyncMqttClient _mqttClient;
TimerHandle_t mqttReconnectTimer;
DNSServer _dnsServer;
WebServer _webServer(80);
HTTPUpdateServer _httpUpdater;
IotWebConf _iotWebConf(TAG, &_dnsServer, &_webServer, TAG, CONFIG_VERSION);

char _classicIP[IOTWEBCONF_WORD_LEN];
char _classicPort[PORT_CONFIG_LEN];
char _mqttServer[IOTWEBCONF_WORD_LEN];
char _mqttPort[PORT_CONFIG_LEN];
char _mqttUserName[IOTWEBCONF_WORD_LEN];
char _mqttUserPassword[IOTWEBCONF_WORD_LEN];
char _mqttRootTopic[IOTWEBCONF_WORD_LEN];
char _willTopic[IOTWEBCONF_WORD_LEN];
IotWebConfParameter classicIPParam = IotWebConfParameter("Classic IP", "classicIP", _classicIP, IOTWEBCONF_WORD_LEN);
IotWebConfParameter classicPortParam = IotWebConfParameter("Classic port", "classicPort", _classicPort, PORT_CONFIG_LEN, "text", NULL, "502");
IotWebConfSeparator MQTT_seperatorParam = IotWebConfSeparator("MQTT");
IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", _mqttServer, IOTWEBCONF_WORD_LEN);
IotWebConfParameter mqttPortParam = IotWebConfParameter("MQTT port", "mqttSPort", _mqttPort, PORT_CONFIG_LEN, "text", NULL, "1883");
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", _mqttUserName, IOTWEBCONF_WORD_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", _mqttUserPassword, IOTWEBCONF_WORD_LEN, "password");

unsigned long _lastPublishTimeStamp = 0;
unsigned long _lastModbusPollTimeStamp = 0;
unsigned long _publishRate = SNOOZE_PUBLISH_RATE;
int _publishCount = 0;
uint32_t boilerPlatePollRate = 0;
String fullTopic_PUB;
bool mqttReadingsAvailable = false;
bool boilerPlateInfoPublished = false;
uint8_t boilerPlateReadBitField = 0;
ChargeControllerInfo _chargeControllerInfo;
esp32ModbusTCP *_pClassic;

int _currentRegister = 0;

#define numBanks (sizeof(_registers) / sizeof(ModbusRegisterBank))
ModbusRegisterBank _registers[] = {
	{false, 4100, 44},
	{false, 4360, 22},
	{false, 4163, 2},
	{false, 4209, 4},
	{false, 4243, 32}
	//{ false, 16386, 8 }
};

void publish(const char *subtopic, const char *value, boolean retained = false)
{
	if (_mqttClient.connected())
	{
		char buf[64];
		sprintf(buf, "%s/classic/stat/%s", _mqttRootTopic, subtopic);
		_mqttClient.publish(buf, 0, retained, value);
	}
}

// note, add 0.01 as a work around for Android JSON deserialization bug with float
void publishReadings()
{
	StaticJsonDocument<1024> root;
	if ((boilerPlateReadBitField & 0x0f) == 0x0f && boilerPlateInfoPublished == false)
	{
		boilerPlateInfoPublished = true;

		root["appVersion"] = _chargeControllerInfo.appVersion;
		root["buildDate"] = _chargeControllerInfo.buildDate;
		root["deviceName"] = _chargeControllerInfo.deviceName;
		root["deviceType"] = "Classic";
		root["endingAmps"] = _chargeControllerInfo.endingAmps + 0.01;
		root["hasWhizbang"] = _chargeControllerInfo.hasWhizbang;
		root["lastVOC"] = _chargeControllerInfo.lastVOC + 0.01;
		root["model"] = _chargeControllerInfo.model;
		root["mpptMode"] = _chargeControllerInfo.mpptMode;
		root["netVersion"] = _chargeControllerInfo.netVersion;
		root["nominalBatteryVoltage"] = _chargeControllerInfo.nominalBatteryVoltage;
		root["unitID"] = _chargeControllerInfo.unitID;
		String s;
		serializeJson(root, s);
		publish("info", s.c_str());
	}
	root.clear();
	root["BatTemperature"] = _chargeControllerInfo.BatTemperature + 0.01;
	root["NetAmpHours"] = _chargeControllerInfo.NetAmpHours;
	root["ChargeState"] = _chargeControllerInfo.ChargeState;
	root["InfoFlagsBits"] = _chargeControllerInfo.InfoFlagsBits;
	root["ReasonForResting"] = _chargeControllerInfo.ReasonForResting;
	root["NegativeAmpHours"] = _chargeControllerInfo.NegativeAmpHours;
	root["BatVoltage"] = _chargeControllerInfo.BatVoltage + 0.01;
	root["PVVoltage"] = _chargeControllerInfo.PVVoltage + 0.01;
	root["VbattRegSetPTmpComp"] = _chargeControllerInfo.VbattRegSetPTmpComp;
	root["TotalAmpHours"] = _chargeControllerInfo.TotalAmpHours;
	root["WhizbangBatCurrent"] = _chargeControllerInfo.WhizbangBatCurrent + 0.01;
	root["BatCurrent"] = _chargeControllerInfo.BatCurrent + 0.01;
	root["PVCurrent"] = _chargeControllerInfo.PVCurrent + 0.01;
	root["ConnectionState"] = 0;
	root["EnergyToday"] = _chargeControllerInfo.EnergyToday + 0.01;
	root["EqualizeTime"] = _chargeControllerInfo.EqualizeTime;
	root["SOC"] = _chargeControllerInfo.SOC;
	root["Aux1"] = _chargeControllerInfo.Aux1;
	root["Aux2"] = _chargeControllerInfo.Aux2;
	root["Power"] = _chargeControllerInfo.Power + 0.01;
	root["FETTemperature"] = _chargeControllerInfo.FETTemperature + 0.01;
	root["PositiveAmpHours"] = _chargeControllerInfo.PositiveAmpHours;
	root["TotalEnergy"] = _chargeControllerInfo.TotalEnergy + 0.01;
	root["FloatTimeTodaySeconds"] = _chargeControllerInfo.FloatTimeTodaySeconds;
	root["RemainingAmpHours"] = _chargeControllerInfo.RemainingAmpHours;
	root["AbsorbTime"] = _chargeControllerInfo.AbsorbTime;
	root["ShuntTemperature"] = _chargeControllerInfo.ShuntTemperature + 0.01;
	root["PCBTemperature"] = _chargeControllerInfo.PCBTemperature + 0.01;
	String s;
	serializeJson(root, s);
	publish("readings", s.c_str());
}

uint16_t Getuint16Value(int index, uint8_t *data)
{
	index *= 2;
	return (data[index] << 8 | data[index + 1]);
}

int16_t Getint16Value(int index, uint8_t *data)
{
	index *= 2;
	return (data[index] << 8 | data[index + 1]);
}

uint32_t Getuint32Value(int index, uint8_t *data)
{
	index *= 2;
	return data[index + 2] << 24 | data[index + 3] << 16 | data[index] << 8 | data[index + 1];
}

int32_t Getint32Value(int index, uint8_t *data)
{
	index *= 2;
	return data[index + 2] << 24 | data[index + 3] << 16 | data[index] << 8 | data[index + 1];
}

float GetFloatValue(int index, uint8_t *data, float div = 1.0)
{
	return Getint16Value(index, data) / div;
}

uint8_t GetMSBValue(int index, uint8_t *data)
{
	index *= 2;
	return (data[index] >> 8);
}

boolean GetFlagValue(int index, uint16_t mask, uint8_t *data)
{
	index *= 2;
	return (data[index] & mask) != 0;
}

void readModbus()
{
	if (_currentRegister < numBanks)
	{
		if (_registers[_currentRegister].received == false)
		{
			if (_pClassic->readHoldingRegisters(_registers[_currentRegister].address, _registers[_currentRegister].byteCount) != 0)
			{
				logi("Requesting %d for %d bytes\n", _registers[_currentRegister].address, _registers[_currentRegister].byteCount);
			}
			else
			{
				loge("Request %d failed\n", _registers[_currentRegister].address);
			}
		}
		_currentRegister++;
	}
}

void SetBankReceived(uint16_t byteCount)
{
	int regCount = byteCount / 2;
	for (int i = 0; i < numBanks; i++)
	{
		if (_registers[i].byteCount == regCount)
		{
			_registers[i].received = true;
		}
	}
}

//void ClearAllReceived() {
//	boilerPlateReadBitField = 0;
//	for (int i = 0; i < numBanks; i++) {
//		_registers[i].received = false;
//	}
//}

void Wake()
{
	_publishRate = WAKE_PUBLISH_RATE;
	_lastPublishTimeStamp = 0;
	_lastModbusPollTimeStamp = 0;
}

void modbusErrorCallback(uint16_t packetId, MBError error)
{
	String text;
	switch (error)
	{
	case 0x00:
		text = "SUCCESS";
		break;
	case 0x01:
		text = "ILLEGAL_FUNCTION";
		break;
	case 0x02:
		text = "ILLEGAL_DATA_ADDRESS";
		break;
	case 0x03:
		text = "ILLEGAL_DATA_VALUE";
		break;
	case 0x04:
		text = "SERVER_DEVICE_FAILURE";
		break;
	case 0x05:
		text = "ACKNOWLEDGE";
		break;
	case 0x06:
		text = "SERVER_DEVICE_BUSY";
		break;
	case 0x07:
		text = "NEGATIVE_ACKNOWLEDGE";
		break;
	case 0x08:
		text = "MEMORY_PARITY_ERROR";
		break;
	case 0xE0:
		text = "TIMEOUT";
		break;
	case 0xE1:
		text = "INVALID_SLAVE";
		break;
	case 0xE2:
		text = "INVALID_FUNCTION";
		break;
	case 0xE3:
		text = "CRC_ERROR";
		break;
	case 0xE4:
		text = "COMM_ERROR";
		break;
	}
	loge("packetId[0x%x], error[%s]", packetId, text);
}

void modbusCallback(uint16_t packetId, uint8_t slaveAddress, MBFunctionCode functionCode, uint8_t *data, uint16_t byteCount)
{
	logd("packetId[0x%x], slaveAddress[0x%x], functionCode[0x%x], byteCount[%d]", packetId, slaveAddress, functionCode, byteCount);
	SetBankReceived(byteCount);
	if (byteCount == 88)
	{
		_chargeControllerInfo.BatVoltage = GetFloatValue(14, data, 10.0);
		_chargeControllerInfo.PVVoltage = GetFloatValue(15, data, 10.0);
		_chargeControllerInfo.BatCurrent = GetFloatValue(16, data, 10.0);
		_chargeControllerInfo.EnergyToday = GetFloatValue(17, data, 10.0);
		_chargeControllerInfo.Power = GetFloatValue(18, data);
		_chargeControllerInfo.ChargeState = GetMSBValue(19, data);
		_chargeControllerInfo.PVCurrent = GetFloatValue(20, data, 10.0);
		_chargeControllerInfo.TotalEnergy = Getuint32Value(25, data) / 10.0;
		_chargeControllerInfo.InfoFlagsBits = Getuint32Value(29, data);
		_chargeControllerInfo.BatTemperature = GetFloatValue(31, data, 10.0);
		_chargeControllerInfo.FETTemperature = GetFloatValue(32, data, 10.0);
		_chargeControllerInfo.PCBTemperature = GetFloatValue(33, data, 10.0);
		_chargeControllerInfo.FloatTimeTodaySeconds = Getuint16Value(37, data);
		_chargeControllerInfo.AbsorbTime = Getuint16Value(38, data);
		_chargeControllerInfo.EqualizeTime = Getuint16Value(42, data);
		_chargeControllerInfo.Aux1 = GetFlagValue(29, 0x4000, data);
		_chargeControllerInfo.Aux2 = GetFlagValue(29, 0x8000, data);

		if ((boilerPlateReadBitField & 0x1) == 0)
		{
			boilerPlateReadBitField |= 0x1;
			uint16_t reg1 = Getuint16Value(0, data);
			char buf[32];
			sprintf(buf, "Classic %d (rev %d)", reg1 & 0x00ff, reg1 >> 8);
			_chargeControllerInfo.model = buf;
			int buildYear = Getuint16Value(1, data);
			int buildMonthDay = Getuint16Value(2, data);
			sprintf(buf, "%d%02d%02d", buildYear, (buildMonthDay >> 8), (buildMonthDay & 0x00ff));
			_chargeControllerInfo.buildDate = buf;
			_chargeControllerInfo.lastVOC = GetFloatValue(21, data, 10.0);
			_chargeControllerInfo.unitID = Getuint32Value(10, data);
		}
	}
	else if (byteCount == 44)
	{ // whizbang readings
		_chargeControllerInfo.PositiveAmpHours = Getuint32Value(4, data);
		_chargeControllerInfo.NegativeAmpHours = abs(Getuint32Value(6, data));
		_chargeControllerInfo.NetAmpHours = 0; //Getint32Value(8, data); // todo causing deserialization exception in android
		_chargeControllerInfo.ShuntTemperature = (Getuint16Value(11, data) & 0x00ff) - 50.0f;
		_chargeControllerInfo.WhizbangBatCurrent = GetFloatValue(10, data, 10.0);
		_chargeControllerInfo.SOC = Getuint16Value(12, data);
		_chargeControllerInfo.RemainingAmpHours = Getuint16Value(16, data);
		_chargeControllerInfo.TotalAmpHours = Getuint16Value(20, data);
	}
	else if (byteCount == 4)
	{ // boilerplate data
		if ((boilerPlateReadBitField & 0x02) == 0)
		{
			boilerPlateReadBitField |= 0x02;
			_chargeControllerInfo.mpptMode = Getuint16Value(0, data);
			int Aux12FunctionS = (Getuint16Value(1, data) & 0x3f00) >> 8;
			_chargeControllerInfo.hasWhizbang = Aux12FunctionS == 18;
		}
	}
	else if (byteCount == 8)
	{
		if ((boilerPlateReadBitField & 0x04) == 0)
		{
			boilerPlateReadBitField |= 0x04;
			char unit[9];
			unit[0] = data[1];
			unit[1] = data[0];
			unit[2] = data[3];
			unit[3] = data[2];
			unit[4] = data[5];
			unit[5] = data[4];
			unit[6] = data[7];
			unit[7] = data[6];
			unit[8] = 0;
			_chargeControllerInfo.deviceName = unit;
		}
	}
	else if (byteCount == 64)
	{
		if ((boilerPlateReadBitField & 0x08) == 0)
		{
			boilerPlateReadBitField |= 0x08;
			_chargeControllerInfo.VbattRegSetPTmpComp = GetFloatValue(0, data, 10.0);
			_chargeControllerInfo.nominalBatteryVoltage = Getuint16Value(1, data);
			_chargeControllerInfo.endingAmps = GetFloatValue(2, data, 10.0);
			_chargeControllerInfo.ReasonForResting = Getuint16Value(31, data);
		}
	}
	else if (byteCount == 16)
	{
		if ((boilerPlateReadBitField & 0x10) == 0)
		{
			boilerPlateReadBitField |= 0x10;
			short reg16387 = Getuint16Value(0, data);
			short reg16388 = Getuint16Value(1, data);
			short reg16389 = Getuint16Value(2, data);
			short reg16390 = Getuint16Value(3, data);
			char unit[16];
			snprintf_P(unit, sizeof(unit), "%d", (reg16388 << 16) + reg16387);
			_chargeControllerInfo.appVersion = unit;
			snprintf_P(unit, sizeof(unit), "%d", (reg16390 << 16) + reg16389);
			_chargeControllerInfo.netVersion = unit;
		}
	}
}

void onMqttConnect(bool sessionPresent)
{
	logi("Connected to MQTT. Session present: %d", sessionPresent);
	char buf[64];
	sprintf(buf, "%s/classic/cmnd/#", _mqttRootTopic);
	_mqttClient.subscribe(buf, 0);
	_mqttClient.publish(_willTopic, 0, true, "Online");
	logi("Subscribed to [%s], qos: 0", buf);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
	logi("Disconnected from MQTT. Reason: %d", (int8_t)reason);
	if (WiFi.isConnected())
	{
		xTimerStart(mqttReconnectTimer, 0);
	}
}

void connectToMqtt()
{
	if (WiFi.isConnected())
	{
		if (strlen(_mqttServer) > 0) // mqtt configured
		{
			logi("Connecting to MQTT...");
			_mqttClient.connect();
		}
	}
}

/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
	// -- Let IotWebConf test and handle captive portal requests.
	if (_iotWebConf.handleCaptivePortal())
	{
		// -- Captive portal request were already served.
		return;
	}
	String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
	s += "<title>ClassicMQTT</title></head><body>";
	s += _iotWebConf.getThingName();
	s += "<ul>";
	s += "<li>Classic IP: ";
	s += _classicIP;
	s += "</ul>";
	s += "<ul>";
	s += "<li>Classic port: ";
	s += _classicPort;
	s += "</ul>";
	s += "<ul>";
	s += "<li>MQTT server: ";
	s += _mqttServer;
	s += "</ul>";
	s += "<ul>";
	s += "<li>MQTT port: ";
	s += _mqttPort;
	s += "</ul>";
	s += "<ul>";
	s += "<li>MQTT user: ";
	s += _mqttUserName;
	s += "</ul>";
	s += "<ul>";
	s += "<li>MQTT root topic: ";
	s += _mqttRootTopic;
	s += "</ul>";
	s += "Go to <a href='config'>configure page</a> to change values.";
	s += "</body></html>\n";
	_webServer.send(200, "text/html", s);
}

void configSaved()
{
	logi("Configuration was updated.");
}

boolean formValidator()
{
	boolean valid = true;
	int mqttServerParamLength = _webServer.arg(mqttServerParam.getId()).length();
	if (mqttServerParamLength == 0)
	{
		mqttServerParam.errorMessage = "MQTT server is required";
		valid = false;
	}
	return valid;
}

void WiFiEvent(WiFiEvent_t event)
{
	logi("[WiFi-event] event: %d", event);
	switch (event)
	{
	case SYSTEM_EVENT_STA_GOT_IP:
		logi("WiFi connected, IP address: %s", WiFi.localIP().toString().c_str());
		xTimerStart(mqttReconnectTimer, 0);
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		logi("WiFi lost connection");
		xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
		break;
	default:
		break;
	}
}

void onMqttPublish(uint16_t packetId)
{
	logi("Publish acknowledged.  packetId: %d", packetId);
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
	logd("MQTT Message arrived [%s]  qos: %d len: %d index: %d total: %d", topic, properties.qos, len, index, total);
	printHexString(payload, len);
	char pl[16];
	for (int i = 0; i < len; i++)
	{
		pl[i] = toupper(payload[i]);
	}
	if (strncmp(pl, "{\"WAKE\"}", len) == 0)
	{
		boilerPlateInfoPublished = false;
		Wake();
		logd("Wake poll rate");
	}
	else if (strncmp(pl, "{\"INFO\"}", len) == 0)
	{
		boilerPlateInfoPublished = false;
		Wake();
		logd("info request received");
	}
	else
	{
		logd("MQTT Message {%s} not recognized!", pl);
	}
}

void setup()
{
	Serial.begin(115200);
	while (!Serial)
	{
		; // wait for serial port to connect. Needed for native USB port only
	}
	logi("Booting");
	pinMode(WIFI_AP_PIN, INPUT_PULLUP);
	pinMode(WIFI_STATUS_PIN, OUTPUT);
	mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(5000), pdFALSE, (void *)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
	WiFi.onEvent(WiFiEvent);
	_iotWebConf.setStatusPin(WIFI_STATUS_PIN);
	_iotWebConf.setConfigPin(WIFI_AP_PIN);
	// setup EEPROM parameters
	_iotWebConf.addParameter(&classicIPParam);
	_iotWebConf.addParameter(&classicPortParam);
	_iotWebConf.addParameter(&MQTT_seperatorParam);
	_iotWebConf.addParameter(&mqttServerParam);
	_iotWebConf.addParameter(&mqttPortParam);
	_iotWebConf.addParameter(&mqttUserNameParam);
	_iotWebConf.addParameter(&mqttUserPasswordParam);

	// setup callbacks for IotWebConf
	_iotWebConf.setConfigSavedCallback(&configSaved);
	_iotWebConf.setFormValidator(&formValidator);
	_iotWebConf.setupUpdateServer(&_httpUpdater);
	boolean validConfig = _iotWebConf.init();
	if (!validConfig)
	{
		loge("!invalid configuration!");
		_classicIP[0] = '\0';
		_classicPort[0] = '\0';
		_mqttServer[0] = '\0';
		_mqttPort[0] = '\0';
		_mqttUserName[0] = '\0';
		_mqttUserPassword[0] = '\0';
		_iotWebConf.resetWifiAuthInfo();
	}
	else
	{
		_iotWebConf.setApTimeoutMs(AP_TIMEOUT);
		_mqttClient.onConnect(onMqttConnect);
		_mqttClient.onDisconnect(onMqttDisconnect);
		_mqttClient.onMessage(onMqttMessage);
		_mqttClient.onPublish(onMqttPublish);
		sprintf(_mqttRootTopic, "%s", _iotWebConf.getThingName());
		sprintf(_willTopic, "%s/tele/LWT", _mqttRootTopic);
		IPAddress ip;
		if (ip.fromString(_mqttServer))
		{
			int port = atoi(_mqttPort);
			_mqttClient.setServer(ip, port);
			_mqttClient.setCredentials(_mqttUserName, _mqttUserPassword);
			_mqttClient.setWill(_willTopic, 0, true, "Offline");
		}
		if (ip.fromString(_classicIP))
		{
			int port = atoi(_classicPort);
			_pClassic = new esp32ModbusTCP(10, ip, port);
			_pClassic->onData(modbusCallback);
			_pClassic->onError(modbusErrorCallback);
		}
	}
	// Set up required URL handlers on the web server.
	_webServer.on("/", handleRoot);
	_webServer.on("/config", [] { _iotWebConf.handleConfig(); });
	_webServer.onNotFound([]() { _iotWebConf.handleNotFound(); });
	_lastPublishTimeStamp = millis() + MODBUS_POLL_RATE;
	boilerPlatePollRate = millis();
	logi("Done setup");
}

void loop()
{
	_iotWebConf.doLoop();
	if (WiFi.isConnected())
	{
		if (_lastModbusPollTimeStamp < millis())
		{
			_lastModbusPollTimeStamp = millis() + MODBUS_POLL_RATE;
			readModbus();
			if (_currentRegister >= numBanks)
			{
				_currentRegister = 0;
				_registers[0].received = false; // repeat readings
				_registers[1].received = false;
			}
		}
		if (_mqttClient.connected())
		{
			if (_lastPublishTimeStamp < millis())
			{
				_lastPublishTimeStamp = millis() + _publishRate;
				_publishCount++;
				publishReadings();
			}
			if (_publishCount >= WAKE_COUNT)
			{
				_publishCount = 0;
				_publishRate = SNOOZE_PUBLISH_RATE;
			}
		}
	}
}
