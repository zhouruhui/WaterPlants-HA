#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>  // 添加OTA库

// 保持原有的引脚定义
#define MotorSW_PIN 13       //灌溉马达开关
#define WaterSW0_PIN 26      //电磁阀开关0
#define WaterSW1_PIN 27      //电磁阀开关1
#define HumUpdate0_PIN 34   //土壤湿度0采集
#define HumUpdate1_PIN 35   //土壤湿度1采集
#define TempUpdate_PIN 16   //环境温度采集
#define SolarVol_PIN 39     //电池电压采集
#define HaveWater_PIN 25    //灌溉桶液位采集
#define NoWaterLED_PIN 5    //缺水指示灯
#define NoWiFiLED_PIN 0    //断网指示灯

// WiFi配置
const char* WIFI_SSID = "WB_2G";
const char* WIFI_PASSWORD = "Pp@123456";

// MQTT配置
const char* MQTT_SERVER = "192.168.1.102";  // Home Assistant的IP地址
const int MQTT_PORT = 1883;       // MQTT端口，默认1883
const char* MQTT_USER = "summer";    // MQTT用户名
const char* MQTT_PASSWORD = "Pp@123456"; // MQTT密码
const char* DEVICE_NAME = "water_plants"; // 设备名称
const char* OTA_PASSWORD = "plantadmin";  // OTA更新密码
const char* SW_VERSION = "1.0.1";         // 软件版本

// 保持原有的默认值设置
int MotorSW_state = 0;               //默认关闭灌溉马达，0-关闭，1-开启
int WaterMode_state = 1;             //默认手动灌溉模式，0-自动，1-手动
int WaterSW0_state = 0;              //默认关闭电磁阀0，0-关闭，1-开启
int WaterSW1_state = 0;              //默认关闭电磁阀1，0-关闭，1-开启
int HumLevel0_state = 10;            //默认湿度阈值0，范围0~100，步长1
int HumLevel1_state = 10;            //默认湿度阈值1，范围0~100，步长1
int WaterTime0_state = 15;           //默认灌溉时长0，范围1~60s，步长1
int WaterTime1_state = 10;           //默认灌溉时长1，范围1~60s，步长1
int UpdateTime_state = 10;           //默认数据上传间隔，范围1~600s，步长1

// MQTT主题定义
const char* MQTT_DISCOVERY_PREFIX = "homeassistant";
const char* MQTT_STATE_TOPIC = "homeassistant/water_plants/state";
const char* MQTT_COMMAND_TOPIC_PREFIX = "homeassistant/water_plants";

WiFiClient espClient;
PubSubClient client(espClient);

// 保持原有的传感器对象
OneWire DS18B20(TempUpdate_PIN);
DallasTemperature sensors(&DS18B20);

// 保持原有的定时器变量
unsigned long previousWaterMillis0 = 0;
unsigned long previousWaterMillis1 = 0;
bool isWatering0 = 0;
bool isWatering1 = 0;

// 保持原有的传感器读取函数
float getTemperature() {
  // 检查温度传感器是否存在并正常工作
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  
  if (tempC == DEVICE_DISCONNECTED_C || tempC == -127.00) {
    Serial.println("温度传感器未连接或工作异常");
    // 在这里您可以选择返回一个固定值替代NAN
    // 例如返回25.0作为默认室温值
    return NAN; // 如果您想发送替代值，可以改为 return 25.0;
  }
  
  return tempC;
}

const float referenceVoltage = 3.3;
const float humidityVoltageScale = 1.47;
float getHumidity(int sensorPin) {
    int analogValue = analogRead(sensorPin);
    float voltage = (analogValue * (referenceVoltage / 4095.0)) * humidityVoltageScale;
    float humidity = 0.0;

    if (voltage >= 4.8) {
        humidity = 0.0;
    } else if (voltage >= 3.8) {
        humidity = 0.0 + (20.0 - 0.0) * (4.8 - voltage) / (4.8 - 3.8);
    } else if (voltage >= 2.7) {
        humidity = 20.0 + (40.0 - 20.0) * (3.8 - voltage) / (3.8 - 2.7);
    } else if (voltage >= 1.8) {
        humidity = 40.0 + (60.0 - 40.0) * (2.7 - voltage) / (2.7 - 1.8);
    } else if (voltage >= 1.2) {
        humidity = 60.0 + (80.0 - 60.0) * (1.8 - voltage) / (1.8 - 1.2);
    } else if (voltage >= 0.8) {
        humidity = 80.0 + (100.0 - 80.0) * (1.2 - voltage) / (1.2 - 0.8);
    } else {
        humidity = 100.0;
    }

    return humidity;
}

const float solarVoltageScale = 4.0;
float getsolarVol(int sensorPin) {
    int analogValue = analogRead(sensorPin);
    float voltage = analogValue * (referenceVoltage / 4095.0);
    float actualVoltage = voltage * solarVoltageScale;
    return actualVoltage;
}

// 保持原有的灌溉控制函数
void delayforWaterTime0(unsigned long seconds) {
  unsigned long currentWaterMillis = millis();
  unsigned long intervalMillis = seconds * 1000;
  if (isWatering0 && (currentWaterMillis - previousWaterMillis0 >= intervalMillis)) {
    digitalWrite(MotorSW_PIN, LOW);
    digitalWrite(WaterSW0_PIN, LOW);
    isWatering0 = 0;
    Serial.println("Shutdown watering0");
  }
}

void delayforWaterTime1(unsigned long seconds) {
  unsigned long currentWaterMillis = millis();
  unsigned long intervalMillis = seconds * 1000;
  if (isWatering1 && (currentWaterMillis - previousWaterMillis1 >= intervalMillis)) {
    digitalWrite(MotorSW_PIN, LOW);
    digitalWrite(WaterSW1_PIN, LOW);
    isWatering1 = 0;
    Serial.println("Shutdown watering1");
  }
}

void startWatering0() {
  digitalWrite(MotorSW_PIN, HIGH);
  digitalWrite(WaterSW0_PIN, HIGH);
  previousWaterMillis0 = millis();
  isWatering0 = 1;
  Serial.println("startWatering0");
}

void startWatering1() {
  digitalWrite(MotorSW_PIN, HIGH);
  digitalWrite(WaterSW1_PIN, HIGH);
  previousWaterMillis1 = millis();
  isWatering1 = 1;
  Serial.println("startWatering1");
}

// 发布状态
void publishState() {
  Serial.println("发布当前状态...");
  
  // 分批次发布状态
  // 第一批：开关状态
  StaticJsonDocument<256> doc1;
  doc1["motor_sw"] = MotorSW_state;
  doc1["water_mode"] = WaterMode_state;
  doc1["water_sw0"] = WaterSW0_state;
  doc1["water_sw1"] = WaterSW1_state;
  
  String jsonString1;
  serializeJson(doc1, jsonString1);
  Serial.print("状态JSON (开关): ");
  Serial.println(jsonString1);
  bool published1 = client.publish((String(MQTT_STATE_TOPIC) + "/switches").c_str(), jsonString1.c_str(), true);
  if (published1) {
    Serial.println("开关状态发布成功");
  } else {
    Serial.println("开关状态发布失败");
  }
  
  delay(50); // 短暂延迟，避免消息拥堵
  
  // 第二批：阈值状态
  StaticJsonDocument<256> doc2;
  doc2["hum_level0"] = HumLevel0_state;
  doc2["hum_level1"] = HumLevel1_state;
  doc2["water_time0"] = WaterTime0_state;
  doc2["water_time1"] = WaterTime1_state;
  doc2["update_time"] = UpdateTime_state;
  
  String jsonString2;
  serializeJson(doc2, jsonString2);
  Serial.print("状态JSON (阈值): ");
  Serial.println(jsonString2);
  bool published2 = client.publish((String(MQTT_STATE_TOPIC) + "/thresholds").c_str(), jsonString2.c_str(), true);
  if (published2) {
    Serial.println("阈值状态发布成功");
  } else {
    Serial.println("阈值状态发布失败");
  }
  
  delay(50); // 短暂延迟，避免消息拥堵
  
  // 第三批：传感器状态
  StaticJsonDocument<256> doc3;
  doc3["humidity0"] = getHumidity(HumUpdate0_PIN);
  doc3["humidity1"] = getHumidity(HumUpdate1_PIN);
  
  // 尝试获取温度，如果读取失败则不添加到JSON中
  float temperature = getTemperature();
  if (!isnan(temperature)) {
    doc3["temperature"] = temperature;
  }
  
  doc3["solar_voltage"] = getsolarVol(SolarVol_PIN);
  doc3["have_water"] = digitalRead(HaveWater_PIN);
  doc3["wifi_rssi"] = WiFi.RSSI();
  
  String jsonString3;
  serializeJson(doc3, jsonString3);
  Serial.print("状态JSON (传感器): ");
  Serial.println(jsonString3);
  bool published3 = client.publish((String(MQTT_STATE_TOPIC) + "/sensors").c_str(), jsonString3.c_str(), true);
  if (published3) {
    Serial.println("传感器状态发布成功");
  } else {
    Serial.println("传感器状态发布失败");
  }
  
  // 同时也尝试发布一个简化的完整状态
  StaticJsonDocument<512> doc4;
  doc4["motor_sw"] = MotorSW_state;
  doc4["water_mode"] = WaterMode_state;
  doc4["water_sw0"] = WaterSW0_state;
  doc4["water_sw1"] = WaterSW1_state;
  doc4["hum_level0"] = HumLevel0_state;
  doc4["hum_level1"] = HumLevel1_state;
  doc4["humidity0"] = getHumidity(HumUpdate0_PIN);
  doc4["humidity1"] = getHumidity(HumUpdate1_PIN);
  
  // 也在简化状态中检查温度
  if (!isnan(temperature)) {
    doc4["temperature"] = temperature;
  }
  
  doc4["have_water"] = digitalRead(HaveWater_PIN);
  doc4["solar_voltage"] = getsolarVol(SolarVol_PIN);
  
  String jsonString4;
  serializeJson(doc4, jsonString4);
  Serial.print("简化全状态JSON: ");
  Serial.println(jsonString4);
  bool published4 = client.publish(MQTT_STATE_TOPIC, jsonString4.c_str(), true);
  if (published4) {
    Serial.println("简化全状态发布成功");
  } else {
    Serial.println("简化全状态发布失败");
  }
}

// MQTT回调函数
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("Received message on topic: ");
  Serial.println(topic);
  Serial.print("Message: ");
  Serial.println(message);
  
  // 从主题中提取命令类型
  String topicStr = String(topic);
  
  if (topicStr.endsWith("/motor_sw/set")) {
    MotorSW_state = (message == "1");
    digitalWrite(MotorSW_PIN, MotorSW_state ? HIGH : LOW);
    Serial.print("Motor switch set to: ");
    Serial.println(MotorSW_state);
  } else if (topicStr.endsWith("/water_mode/set")) {
    WaterMode_state = (message == "1");
    Serial.print("Water mode set to: ");
    Serial.println(WaterMode_state);
  } else if (topicStr.endsWith("/water_sw0/set")) {
    WaterSW0_state = (message == "1");
    digitalWrite(WaterSW0_PIN, WaterSW0_state ? HIGH : LOW);
    Serial.print("Water switch 0 set to: ");
    Serial.println(WaterSW0_state);
  } else if (topicStr.endsWith("/water_sw1/set")) {
    WaterSW1_state = (message == "1");
    digitalWrite(WaterSW1_PIN, WaterSW1_state ? HIGH : LOW);
    Serial.print("Water switch 1 set to: ");
    Serial.println(WaterSW1_state);
  } else if (topicStr.endsWith("/hum_level0/set")) {
    HumLevel0_state = message.toInt();
    Serial.print("Humidity level 0 set to: ");
    Serial.println(HumLevel0_state);
  } else if (topicStr.endsWith("/hum_level1/set")) {
    HumLevel1_state = message.toInt();
    Serial.print("Humidity level 1 set to: ");
    Serial.println(HumLevel1_state);
  } else if (topicStr.endsWith("/water_time0/set")) {
    WaterTime0_state = message.toInt();
    Serial.print("Water time 0 set to: ");
    Serial.println(WaterTime0_state);
  } else if (topicStr.endsWith("/water_time1/set")) {
    WaterTime1_state = message.toInt();
    Serial.print("Water time 1 set to: ");
    Serial.println(WaterTime1_state);
  } else if (topicStr.endsWith("/update_time/set")) {
    UpdateTime_state = message.toInt();
    Serial.print("Update time set to: ");
    Serial.println(UpdateTime_state);
  } else {
    Serial.println("Unknown command topic");
    return;
  }
  
  // 立即发布更新后的状态
  publishState();
}

// 发布发现配置
void publishDiscovery() {
  Serial.println("Publishing discovery configuration...");
  
  // 发布开关配置
  publishSwitchDiscovery("motor_sw", "灌溉电机");
  delay(100);
  publishSwitchDiscovery("water_mode", "浇水模式");
  delay(100);
  publishSwitchDiscovery("water_sw0", "浇水开关0");
  delay(100);
  publishSwitchDiscovery("water_sw1", "浇水开关1");
  delay(100);
  
  // 发布传感器配置
  publishSensorDiscovery("humidity0", "土壤湿度0", "%");
  delay(100);
  publishSensorDiscovery("humidity1", "土壤湿度1", "%");
  delay(100);
  publishSensorDiscovery("temperature", "温度", "°C");
  delay(100);
  publishSensorDiscovery("solar_voltage", "电池电压", "V");
  delay(100);
  publishWifiRssiDiscovery("wifi_rssi", "WiFi信号", "dBm");
  delay(100);
  
  // 发布二进制传感器
  publishBinarySensorDiscovery("have_water", "水位状态");
  delay(100);
  
  // 发布数字控制器配置
  publishNumberDiscovery("hum_level0", "湿度阈值0", 0, 100, 1);
  delay(100);
  publishNumberDiscovery("hum_level1", "湿度阈值1", 0, 100, 1);
  delay(100);
  publishNumberDiscovery("water_time0", "灌溉时长0", 1, 60, 1);
  delay(100);
  publishNumberDiscovery("water_time1", "灌溉时长1", 1, 60, 1);
  delay(100);
  publishNumberDiscovery("update_time", "数据上传间隔", 1, 600, 1);
  delay(100);
  
  // 发布初始状态
  publishState();
  Serial.println("Discovery configuration published");
}

// 发布开关类型的发现配置
void publishSwitchDiscovery(const char* id, const char* name) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/switch/water_plants/" + id + "/config";
  StaticJsonDocument<512> doc;
  
  doc["name"] = name;
  doc["unique_id"] = String("water_plants_") + id;
  doc["command_topic"] = String(MQTT_COMMAND_TOPIC_PREFIX) + "/" + id + "/set";
  doc["state_topic"] = String(MQTT_STATE_TOPIC) + "/switches";
  doc["value_template"] = String("{{ value_json.") + id + " }}";
  doc["payload_on"] = "1";
  doc["payload_off"] = "0";
  doc["retain"] = true;
  
  JsonObject device = createDeviceConfig(doc);
  
  String jsonString;
  serializeJson(doc, jsonString);
  Serial.print("Publishing switch discovery for ");
  Serial.print(id);
  Serial.print(" to topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(jsonString);
  
  bool published = client.publish(topic.c_str(), jsonString.c_str(), true);
  if (published) {
    Serial.print("Successfully published discovery for ");
    Serial.println(id);
  } else {
    Serial.print("Failed to publish discovery for ");
    Serial.println(id);
  }
}

// 发布数值类型的发现配置
void publishNumberDiscovery(const char* id, const char* name, int min, int max, int step) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/number/water_plants/" + id + "/config";
  StaticJsonDocument<512> doc;
  
  doc["name"] = name;
  doc["unique_id"] = String("water_plants_") + id;
  doc["command_topic"] = String(MQTT_COMMAND_TOPIC_PREFIX) + "/" + id + "/set";
  doc["state_topic"] = String(MQTT_STATE_TOPIC) + "/thresholds";
  doc["value_template"] = String("{{ value_json.") + id + " | int }}";
  doc["min"] = min;
  doc["max"] = max;
  doc["step"] = step;
  doc["retain"] = true;
  
  JsonObject device = createDeviceConfig(doc);
  
  String jsonString;
  serializeJson(doc, jsonString);
  Serial.print("Publishing number discovery for ");
  Serial.print(id);
  Serial.print(" to topic: ");
  Serial.println(topic);
  
  bool published = client.publish(topic.c_str(), jsonString.c_str(), true);
  if (published) {
    Serial.print("Successfully published discovery for ");
    Serial.println(id);
  } else {
    Serial.print("Failed to publish discovery for ");
    Serial.println(id);
  }
}

// 发布传感器类型的发现配置
void publishSensorDiscovery(const char* id, const char* name, const char* unit) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/water_plants/" + id + "/config";
  StaticJsonDocument<512> doc;
  
  doc["name"] = name;
  doc["unique_id"] = String("water_plants_") + id;
  doc["state_topic"] = String(MQTT_STATE_TOPIC) + "/sensors";
  doc["value_template"] = String("{{ value_json.") + id + " | float }}";
  doc["unit_of_measurement"] = unit;
  doc["state_class"] = "measurement";
  
  // 为特定传感器类型添加device_class
  if (strcmp(id, "temperature") == 0) {
    doc["device_class"] = "temperature";
  } else if (strcmp(id, "solar_voltage") == 0) {
    doc["device_class"] = "voltage";
  }
  
  JsonObject device = createDeviceConfig(doc);
  
  String jsonString;
  serializeJson(doc, jsonString);
  Serial.print("Publishing sensor discovery for ");
  Serial.print(id);
  Serial.print(" to topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(jsonString);
  
  bool published = client.publish(topic.c_str(), jsonString.c_str(), true);
  if (published) {
    Serial.print("Successfully published sensor discovery for ");
    Serial.println(id);
  } else {
    Serial.print("Failed to publish sensor discovery for ");
    Serial.println(id);
  }
}

// 发布二进制传感器类型的发现配置
void publishBinarySensorDiscovery(const char* id, const char* name) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/binary_sensor/water_plants/" + id + "/config";
  StaticJsonDocument<512> doc;
  
  doc["name"] = name;
  doc["unique_id"] = String("water_plants_") + id;
  doc["state_topic"] = String(MQTT_STATE_TOPIC) + "/sensors";
  doc["value_template"] = String("{{ value_json.") + id + " }}";
  doc["payload_on"] = "1";
  doc["payload_off"] = "0";
  doc["device_class"] = "moisture";
  
  // 特殊处理水位状态
  if (strcmp(id, "have_water") == 0) {
    doc["payload_on"] = "1";
    doc["payload_off"] = "0";
    doc["state_on"] = "充足";
    doc["state_off"] = "缺水";
  }
  
  JsonObject device = createDeviceConfig(doc);
  
  String jsonString;
  serializeJson(doc, jsonString);
  Serial.print("Publishing binary sensor discovery for ");
  Serial.print(id);
  Serial.print(": ");
  Serial.println(jsonString);
  bool published = client.publish(topic.c_str(), jsonString.c_str(), true);
  if (published) {
    Serial.print("Successfully published binary sensor discovery for ");
    Serial.println(id);
  } else {
    Serial.print("Failed to publish binary sensor discovery for ");
    Serial.println(id);
  }
}

// 专门为WiFi信号强度创建的发现配置
void publishWifiRssiDiscovery(const char* id, const char* name, const char* unit) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/water_plants/" + id + "/config";
  StaticJsonDocument<512> doc;
  
  doc["name"] = name;
  doc["unique_id"] = String("water_plants_") + id;
  doc["state_topic"] = String(MQTT_STATE_TOPIC) + "/sensors";
  doc["value_template"] = String("{{ value_json.") + id + " }}"; // 不需要float过滤器，因为RSSI本身是整数
  doc["unit_of_measurement"] = unit;
  doc["state_class"] = "measurement";
  doc["device_class"] = "signal_strength";
  
  JsonObject device = createDeviceConfig(doc);
  
  String jsonString;
  serializeJson(doc, jsonString);
  Serial.print("Publishing RSSI sensor discovery for ");
  Serial.print(id);
  Serial.print(" to topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(jsonString);
  
  bool published = client.publish(topic.c_str(), jsonString.c_str(), true);
  if (published) {
    Serial.print("Successfully published RSSI sensor discovery for ");
    Serial.println(id);
  } else {
    Serial.print("Failed to publish RSSI sensor discovery for ");
    Serial.println(id);
  }
}

// 创建共享的设备配置
JsonObject createDeviceConfig(JsonDocument& doc) {
  JsonObject device = doc.createNestedObject("device");
  device["identifiers"].add("water_plants_controller");
  device["name"] = "Water Plants";
  device["model"] = "ESP32 Water Plants Controller";
  device["manufacturer"] = "DIY";
  device["sw_version"] = SW_VERSION;
  return device;
}

// 设置OTA
void setupOTA() {
  // 设置设备名称
  ArduinoOTA.setHostname(DEVICE_NAME);
  
  // 设置密码
  ArduinoOTA.setPassword(OTA_PASSWORD);

  // 回调函数
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }
    // 更新开始前的准备工作
    Serial.println("开始OTA更新 " + type);
    // 关闭所有输出
    digitalWrite(MotorSW_PIN, LOW);
    digitalWrite(WaterSW0_PIN, LOW);
    digitalWrite(WaterSW1_PIN, LOW);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA更新完成");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("更新进度: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA错误[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("认证失败");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("开始失败");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("连接失败");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("接收失败");
    } else if (error == OTA_END_ERROR) {
      Serial.println("结束失败");
    }
  });
  
  // 启动OTA
  ArduinoOTA.begin();
  Serial.println("OTA准备就绪");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
}

// 连接WiFi
void connectWiFi() {
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiAttemptTime = millis();
  const unsigned long wifiTimeout = 60000;

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(NoWiFiLED_PIN, HIGH);
    if (millis() - wifiAttemptTime >= wifiTimeout) {
      Serial.println("WiFi Connection Timeout. Restarting...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  digitalWrite(NoWiFiLED_PIN, LOW);
}

// 连接MQTT
void connectMQTT() {
  if (!client.connected()) {
    Serial.print("Attempting MQTT connection to ");
    Serial.print(MQTT_SERVER);
    Serial.print(":");
    Serial.print(MQTT_PORT);
    Serial.print(" as ");
    Serial.print(MQTT_USER);
    Serial.println("...");
    
    String clientId = "WaterPlants-";
    clientId += String(random(0xffff), HEX);
    Serial.print("ClientID: ");
    Serial.println(clientId);
    
    // 打印MQTT状态码以便调试
    int state = client.state();
    Serial.print("Current MQTT state: ");
    Serial.println(state);
    Serial.print("Meaning: ");
    switch(state) {
      case -4: Serial.println("连接超时"); break;
      case -3: Serial.println("连接丢失"); break;
      case -2: Serial.println("连接失败"); break;
      case -1: Serial.println("连接断开"); break;
      case 0: Serial.println("已连接"); break;
      case 1: Serial.println("连接被拒绝(协议版本错误)"); break;
      case 2: Serial.println("连接被拒绝(客户端ID错误)"); break;
      case 3: Serial.println("连接被拒绝(服务器不可用)"); break;
      case 4: Serial.println("连接被拒绝(用户名/密码错误)"); break;
      case 5: Serial.println("连接被拒绝(未授权)"); break;
      default: Serial.println("未知错误"); break;
    }
    
    // 尝试发布一个小测试消息来测试连接
    bool testConnected = false;
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("MQTT连接成功，尝试发布测试消息...");
      testConnected = client.publish("homeassistant/water_plants/test", "test", false);
      Serial.println(testConnected ? "测试消息发送成功!" : "测试消息发送失败!");
      
      // 判断连接是否真正可用
      if (!testConnected) {
        Serial.println("尽管显示已连接，但无法发布消息。可能存在权限或连接问题，正在重新连接...");
        client.disconnect();
        delay(1000);
        return;
      }
      
      // 订阅所有命令主题
      Serial.println("订阅命令主题:");
      bool sub1 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/motor_sw/set").c_str());
      Serial.println(sub1 ? "已订阅 motor_sw" : "订阅 motor_sw 失败");
      
      bool sub2 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/water_mode/set").c_str());
      Serial.println(sub2 ? "已订阅 water_mode" : "订阅 water_mode 失败");
      
      bool sub3 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/water_sw0/set").c_str());
      Serial.println(sub3 ? "已订阅 water_sw0" : "订阅 water_sw0 失败");
      
      bool sub4 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/water_sw1/set").c_str());
      Serial.println(sub4 ? "已订阅 water_sw1" : "订阅 water_sw1 失败");
      
      bool sub5 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/hum_level0/set").c_str());
      Serial.println(sub5 ? "已订阅 hum_level0" : "订阅 hum_level0 失败");
      
      bool sub6 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/hum_level1/set").c_str());
      Serial.println(sub6 ? "已订阅 hum_level1" : "订阅 hum_level1 失败");
      
      bool sub7 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/water_time0/set").c_str());
      Serial.println(sub7 ? "已订阅 water_time0" : "订阅 water_time0 失败");
      
      bool sub8 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/water_time1/set").c_str());
      Serial.println(sub8 ? "已订阅 water_time1" : "订阅 water_time1 失败");
      
      bool sub9 = client.subscribe((String(MQTT_COMMAND_TOPIC_PREFIX) + "/update_time/set").c_str());
      Serial.println(sub9 ? "已订阅 update_time" : "订阅 update_time 失败");
      
      // 发布发现配置和初始状态
      Serial.println("等待连接稳定后发布发现配置...");
      delay(1000); // 增加延迟，确保连接稳定
      Serial.println("开始发布发现配置...");
      publishDiscovery();
    } else {
      Serial.print("MQTT连接失败，状态码=");
      Serial.print(client.state());
      Serial.println(" 5秒后重试");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n===== ESP32 WaterPlants Controller Starting =====");
  Serial.println("Version: " + String(SW_VERSION));
  
  // 设置引脚模式
  Serial.println("Initializing pins...");
  pinMode(MotorSW_PIN, OUTPUT);
  pinMode(WaterSW0_PIN, OUTPUT);
  pinMode(WaterSW1_PIN, OUTPUT);
  pinMode(NoWaterLED_PIN, OUTPUT);
  pinMode(NoWiFiLED_PIN, OUTPUT);
  pinMode(HaveWater_PIN, INPUT);
  
  // 设置默认状态
  digitalWrite(MotorSW_PIN, LOW);
  digitalWrite(NoWaterLED_PIN, LOW);
  digitalWrite(NoWiFiLED_PIN, LOW);
  Serial.println("Pins initialized");

  // 连接WiFi
  Serial.println("Starting WiFi connection...");
  connectWiFi();

  // 设置OTA
  Serial.println("Setting up OTA...");
  setupOTA();

  // 设置MQTT
  Serial.println("Setting up MQTT client...");
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);
  Serial.print("MQTT Server: ");
  Serial.println(MQTT_SERVER);
  Serial.print("MQTT Port: ");
  Serial.println(MQTT_PORT);

  // 连接MQTT
  Serial.println("Connecting to MQTT broker...");
  connectMQTT();
  
  Serial.println("Setup complete");
}

void loop() {
  // 处理OTA事件
  ArduinoOTA.handle();
  
  static unsigned long lastDebugTime = 0;
  static unsigned long loopCounter = 0;
  loopCounter++;
  
  // 每60秒打印一次循环状态
  if (millis() - lastDebugTime > 60000) {
    Serial.print("Loop running, count: ");
    Serial.print(loopCounter);
    Serial.print(", Uptime: ");
    Serial.print(millis() / 1000);
    Serial.println(" seconds");
    lastDebugTime = millis();
  }
  
  // 检查WiFi连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost, reconnecting...");
    digitalWrite(NoWiFiLED_PIN, HIGH);
    connectWiFi();
  } else {
    digitalWrite(NoWiFiLED_PIN, LOW);
  }
  
  // 处理MQTT
  if (!client.connected()) {
    Serial.println("MQTT disconnected, reconnecting...");
    connectMQTT();
  }
  client.loop();

  // 定时发布状态
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish >= UpdateTime_state * 1000) {
    Serial.print("Time to publish state update (");
    Serial.print(UpdateTime_state);
    Serial.println(" seconds interval)");
    publishState();
    lastPublish = millis();
  }

  // 检查灌溉时间
  delayforWaterTime0(WaterTime0_state);
  delayforWaterTime1(WaterTime1_state);

  // 检查水位并执行灌溉控制
  if (!digitalRead(HaveWater_PIN)) {
    digitalWrite(NoWaterLED_PIN, HIGH);
    digitalWrite(MotorSW_PIN, LOW);
    // 避免频繁打印相同消息
    static unsigned long lastWaterWarning = 0;
    if (millis() - lastWaterWarning > 30000) {
      Serial.println("Oops! No water at all");
      lastWaterWarning = millis();
    }
  } else {
    digitalWrite(NoWaterLED_PIN, LOW);
    if (WaterMode_state == 0) {  // 自动灌溉模式
      float humidity0 = getHumidity(HumUpdate0_PIN);
      float humidity1 = getHumidity(HumUpdate1_PIN);
      
      // 每60秒打印一次传感器状态
      static unsigned long lastSensorDebug = 0;
      if (millis() - lastSensorDebug > 60000) {
        Serial.println("--- Sensor Status ---");
        Serial.print("Humidity 0: ");
        Serial.print(humidity0);
        Serial.print("%, Threshold: ");
        Serial.println(HumLevel0_state);
        
        Serial.print("Humidity 1: ");
        Serial.print(humidity1);
        Serial.print("%, Threshold: ");
        Serial.println(HumLevel1_state);
        
        Serial.print("Water level: ");
        Serial.println(digitalRead(HaveWater_PIN) ? "OK" : "Low");
        
        Serial.print("Solar voltage: ");
        Serial.println(getsolarVol(SolarVol_PIN));
        
        Serial.print("WiFi RSSI: ");
        Serial.println(WiFi.RSSI());
        Serial.println("--------------------");
        
        lastSensorDebug = millis();
      }
      
      if (humidity0 < HumLevel0_state) {
        Serial.print("Humidity0 (");
        Serial.print(humidity0);
        Serial.print("%) is below threshold (");
        Serial.print(HumLevel0_state);
        Serial.println("%), starting watering");
        startWatering0();
      }
      
      if (humidity1 < HumLevel1_state) {
        Serial.print("Humidity1 (");
        Serial.print(humidity1);
        Serial.print("%) is below threshold (");
        Serial.print(HumLevel1_state);
        Serial.println("%), starting watering");
        startWatering1();
      }
    } else if (MotorSW_state == 1) {  // 手动灌溉模式
      if (WaterSW0_state == 1) {
        startWatering0();
      }
      if (WaterSW1_state == 1) {
        startWatering1();
      }
    }
  }
  
  // 添加一个小延迟，避免CPU占用过高
  delay(100);
} 