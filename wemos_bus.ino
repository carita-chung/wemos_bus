#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>
#include <Fonts/FreeMono9pt7b.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#ifndef min
#define min(x,y) (((x)<(y))?(x):(y))
#endif
#ifndef max
#define max(x,y) (((x)>(y))?(x):(y))
#endif

extern "C" {
#include "user_interface.h"
}


#include <ArduinoJson.h>

char SSID[2][16]     = { 
	"-------",
	"--------",
};
char PASSWORD[2][16] = { 
	"--------",
	"-------------",
};

/*
http://webservices.nextbus.com/service/publicXMLFeed?command=routeList&a=ttc
http://webservices.nextbus.com/service/publicXMLFeed?command=routeConfig&a=ttc&r=95
http://webservices.nextbus.com/service/publicXMLFeed?command=predictionsForMultiStops&a=ttc&stops=95|9834

document below explain restful interface for obtaining bus information
https://www.nextbus.com/xmlFeedDocs/NextBusXMLFeed.pdf
substitute below 1. bus service, 2. route number, 3. stop id for customized use

to obtain stop id information one can make a request like below (example Toronto TTC route 95)
http://webservices.nextbus.com/service/publicXMLFeed?command=routeConfig&a=ttc&r=95
*/
#define NEXTBUS	"webservices.nextbus.com"
#define NEXTBUS_REQ	"/service/publicJSONFeed?command=predictionsForMultiStops&a=%s&stops=%d|%d"

const char _request[] =
	"GET /service/publicJSONFeed?command=predictionsForMultiStops&a=%s&stops=%d|%d HTTP/1.1\r\n"
	"User-Agent: ESP8266/0.1\r\n"
	"Accept: */*\r\n"
	"Host: webservices.nextbus.com \r\n"
	"Connection: close\r\n"
	"\r\n";

const char _bus_service[] = "ttc";
const int  _bus_route[] = { 95, 97, 96, };
const int  _bus_stop[] = { 9834, 6206, 970, };
const int  _num_of_stops = 4;

#define SECONDS_TO_MAKE_REQUEST	60
#define TZ_ADJUST_HR	-5			// adjustment to gmt time

static char _response[4096];

static char _route_title[32];
static char _stop_title[128];

typedef struct {
	char branch[8];
	int  secs;
} route;
static route _route[4][5];
static int   _num_routes = 0;

static int   _secs = 0, _secs_update = 0;
static int   _tripped = 0, _ticks = 0;

#define DELAY_NORMAL    (1*60*10)
#define DELAY_ERROR     (10*60*10)

os_timer_t myTimer;

//================================================================================
void timerCallback(void *p) {
	_tripped++;
}

/*
 * Connections:
 * WeMos D1 Mini   Nokia 5110    Description
 * (ESP8266)       PCD8544 LCD
 *
 * D2 (GPIO4)      0 RST         Output from ESP to reset display
 * D1 (GPIO5)      1 CE          Output from ESP to chip select/enable display
 * D6 (GPIO12)     2 DC          Output from display data/command to ESP
 * D7 (GPIO13)     3 Din         Output from ESP SPI MOSI to display data input
 * D5 (GPIO14)     4 Clk         Output from ESP SPI clock
 * 3V3             5 Vcc         3.3V from ESP to display
 * D0 (GPIO16)     6 BL          3.3V to turn backlight on, or PWM
 * G               7 Gnd         Ground
*/
//_______ LCD driving Pins
const int8_t RST_PIN = D2;
const int8_t CE_PIN = D1;
const int8_t DC_PIN = D6;
const int8_t BL_PIN = D0;

const int8_t BUTTON = D3;

Adafruit_PCD8544 display = Adafruit_PCD8544(DC_PIN, CE_PIN, RST_PIN);
 
void setup() {
	Serial.begin(115200);
  Serial.println("\n\nWeMos D1 Mini + Nokia 5110\n");
 
  // Turn LCD backlight on
  //pinMode(BL_PIN, OUTPUT);
  //digitalWrite(BL_PIN, HIGH);
 
  display.begin();
  //display.setContrast(60);  // Adjust for your display
  //Serial.println("Show Adafruit logo bitmap");
 
  // Show the Adafruit logo, which is preloaded into the buffer by their library
  // display.clearDisplay();
  //delay(2000);
 
  display.setTextSize(1);
  display.setTextColor(BLACK);
  Serial.println("Display initialized!");

	while (1) {
		for (uint8_t i=0;i<2;i++) {
			Serial.print(F("Trying ")); Serial.println(SSID[i]);
			display.clearDisplay();
			display.setCursor(0,0);
			display.setCursor(0, 8);
			display.print(SSID[i]);
			display.display();
			uint8_t c=0;
			WiFi.begin(SSID[i], PASSWORD[i]);
			while (WiFi.status() != WL_CONNECTED) {
				delay(500);
				Serial.print(F("."));
				display.print(F("."));
				display.display();
				if (c++ > 20) break;
			}//while
			if (c <= 20) break;
		}//for
		if (WiFi.status() == WL_CONNECTED) break;
	}//while

  display.println("WiFi OK");
	display.println(F("IP address: "));
	display.println(WiFi.localIP());
  display.display();

	Serial.println();
	Serial.println(F("WiFi OK!"));
	Serial.println(WiFi.localIP());

	pinMode(BUTTON, INPUT_PULLUP);

	os_timer_setfn(&myTimer, timerCallback, NULL);
	os_timer_arm(&myTimer, 100, true);

}

void updateContent(void);
bool serviceRequest(uint8_t s);
bool processRequest(char *json);

static uint8_t _which_route = 0;
//================================================================================
void loop() {
	static uint8_t show_route_info = 0;

	while (_tripped) {
		_tripped--;
		if (!digitalRead(BUTTON)) {
			//________ key pressed, wait for release
			while (!digitalRead(BUTTON)) delay(1);
			_which_route++;
			_which_route %= _num_of_stops;
			_tripped = _ticks = _secs_update = 0;
			show_route_info = 2;
		}//if
		//_____________ this happens roughly every seconds, update whatever needs be
		if (!(_ticks%10)) {
			//___________ every second update display (secs) clock, bus schedule
			if (!(_secs_update%SECONDS_TO_MAKE_REQUEST)) serviceRequest(_which_route);
			_secs++;
			_secs_update++;
			if (_secs >= (3600*12)) _secs = 0;	// 12:00
			//_______ update bus schedules
			for (int8_t k=0;k<_num_routes;k++) {
				for (int8_t i=4;i>=0;i--) _route[k][i].secs--;
				if (_route[k][0].secs < 0) {				// bus gone, redo
					serviceRequest(_which_route);
					break;
				}//if
			}//for
			updateContent(show_route_info);
			if (show_route_info) show_route_info--;
		}//if
		_ticks++;
	}//while
	yield();
}

//================================================================================
void updateContent(uint8_t route_info) {
	display.clearDisplay();
	display.setTextSize(0);
	display.setTextColor(BLACK);
	char buf[32];

	uint8_t x=0, y=84-(6*6+4);
	//______________ block to show time at upper right corner
	sprintf(buf, "%2d", (_secs/3600) ? (_secs/3600) : 12);
	display.setCursor(y, 0);
	display.print(buf);
	y += 2 * 6;

	sprintf(buf, "%02d", (_secs/60)%60);
	display.setCursor(84-(4*6+2), 0);
	display.print(buf);

	sprintf(buf, "%02d", _secs%60);
	display.setCursor(84-(2*6), 0);
	display.print(buf);

	display.drawFastVLine(84-(4*6+2)-2, 5, 2, BLACK);
	display.drawFastVLine(84-(2*6)-2, 5, 2, BLACK);

	//______________ sort upcoming buses based on time
	route order[_num_routes*5];
	int8_t n=0;
	for (int8_t k=0;k<_num_routes;k++)
		for (int8_t i=0;i<4;i++) order[n++] = _route[k][i];
	for (int8_t c=0;c<n-1;c++)
		for (int8_t d=0;d<n-c-1;d++)
			if (order[d].secs > order[d+1].secs) {
				route swap = order[d];
				order[d] = order[d+1];
				order[d+1] = swap;
			}//if



	x = y = 0;
	display.setCursor(x, y);
	if (route_info) {
		//______________ show next bus wait time on upper left of screen
		sprintf(buf, "+%d", order[0].secs/60);
		display.print(buf);
		x += (order[0].secs/60) >= 10 ? 6*3 : 6*2;
		display.drawFastVLine(x, 0, 2, BLACK);
		x += 2;
		display.setCursor(x, y);
		sprintf(buf, "%02d", order[0].secs%60);
		display.print(buf);

		//______________ show route name
		x = 0;
		y = 2 * 8;
		display.setCursor(x, y);
		display.println(_route_title);
		display.println(_stop_title);
	}//if
	else {
		//______________ shows route number
		display.print(_bus_route[_which_route]);

		//______________ mark horizontal timeline w/ buses
		display.drawFastHLine(0, 12, 84, BLACK);

		n = _num_routes * 5 - 1;
		while (n--) {
			int mark = order[n].secs / 30;
			if (mark >= 84) continue;
			display.drawFastVLine(mark, 11, 3, BLACK);
			mark -= 3;
			display.fillRect(mark, 10, 7, 5, WHITE);
			display.drawRect(mark, 10, 7, 5, BLACK);
		}//while

		//___________________ addn lines, upcoming bus
		x = 0;
		n = 0;
		y = 2 * 8;
		while (n < 4) {
			if (!*order[n].branch) break;
			x = 0;
			display.setCursor(x, y);
			sprintf(buf, "%-4.4s", order[n].branch);
			display.print(buf);
			x += 4 * 6;
			sprintf(buf, "+%d", order[n].secs/60);
			display.print(buf);
			x += (order[n].secs/60) >= 10 ? 6*3 : 6*2;
			display.drawFastVLine(x, y, 2, BLACK);
			x += 2;
			display.setCursor(x, y);
			sprintf(buf, "%02d", order[n].secs%60);
			display.print(buf);
			n++;
			y += 8;
		}//while
	}//else

	display.display();
}

//================================================================================
bool serviceRequest(uint8_t s) {
	Serial.println(F("\nconnecting to nextbus.com"));
	int respLen = 0;

	WiFiClient httpclient;
	const int httpPort = 80;
	if (!httpclient.connect(NEXTBUS, httpPort)) {
		Serial.println(F("connection failed"));
		//setup();
		//ESP.restart();	/ h/w restart
		ESP.reset();		// s/w reset
		//delay(DELAY_ERROR);
		return false;
	}//if

	char request[512];
	sprintf(request, _request, _bus_service, _bus_route[s], _bus_stop[s]);
	Serial.print(request);
	httpclient.print(request);
	httpclient.flush();

	bool skip_headers = true;
	while (httpclient.connected() || httpclient.available()) {
		if (skip_headers) {
			String aLine = httpclient.readStringUntil('\n');
			// Blank line denotes end of headers
			if (aLine.length() <= 1) skip_headers = false;
		}//if
		else {
			int bytesIn;
			bytesIn = httpclient.read((uint8_t *)&_response[respLen], sizeof(_response) - respLen);
			if (bytesIn > 0) {
				respLen += bytesIn;
				if (respLen > sizeof(_response)) respLen = sizeof(_response);
			}//if
			else if (bytesIn < 0) {
				Serial.print(F("read error "));
				Serial.println(bytesIn);
			}//if
		}//else
		delay(1);
	}//while
	httpclient.stop();

	if (respLen >= sizeof(_response)) {
		Serial.print(F("_response overflow "));
		Serial.println(respLen);
		delay(DELAY_ERROR);
		return false;
	}//if
	_response[respLen++] = '\0';
	//Serial.print(F("respLen "));
	//Serial.println(respLen);
	Serial.println(_response);

	return processRequest(_response);
}

//================================================================================
bool processRequest(char *json) {
	const size_t bufferSize = 4096;
	DynamicJsonDocument doc(bufferSize);
	char *jsonstart = strchr(json, '{');
	if (jsonstart == NULL) {
		Serial.println(F("JSON data missing"));
		return false;
	}//if
	json = jsonstart;

	DeserializationError err = deserializeJson(doc, json);
	if (err) {
		Serial.print(F("deserializeJson() failed with code "));
		Serial.println(err.c_str());
	}//if

	for (uint8_t k=0;k<4;k++)
		for (uint8_t i=0;i<5;i++) {
			*_route[k][i].branch = '\0';;
			_route[k][i].secs = 9999;
		}//for

	_num_routes = 0;
	for (uint8_t k=0;k<4;k++)
		for (uint8_t i=0;i<5;i++) {
			*_route[k][i].branch = '\0';;
			_route[k][i].secs = 9999;
		}//for

	JsonObject predictions = doc["predictions"];
	JsonObject direction = predictions["direction"];
	strcpy(_route_title, predictions["routeTitle"]);
	strcpy(_stop_title, predictions["stopTitle"]);
	int single_route = predictions["direction"].is<JsonArray>() ? 0 : 1;
	for (uint8_t k=0;k<predictions["direction"].size() || single_route;k++) {
		if (!single_route) direction = predictions["direction"][k];
		JsonObject prediction = direction["prediction"];
		int single_bus = direction["prediction"].is<JsonArray>() ? 0 : 1;
		for (uint8_t i=0;i<direction["prediction"].size() || single_bus;i++) {
			if (!single_bus) prediction = direction["prediction"][i];
			_route[k][i].secs = prediction["seconds"];
			strcpy(_route[k][i].branch, prediction["branch"]);
			if (i==0) {
				if (k==0) {
					char dt_str[16];
					strcpy(dt_str, prediction["epochTime"].as<char*>());
					dt_str[strlen(dt_str)-3] = '\0';		// eliminate millisecs
					long dt = atol(dt_str);
					dt += (TZ_ADJUST_HR * 3600);
					_secs = dt % (3600*12);
					_secs -= _route[0][0].secs;
				}//if
				Serial.print(_route[k][i].branch);
				Serial.print(F(": "));
			}//if
			Serial.print(F("+"));
			Serial.print(_route[k][i].secs);
			Serial.print(F(" "));
			if (i==4 || single_bus) break;
		}//for
		Serial.println(F("... "));
		_num_routes++;
		if (_num_routes >= 4 || single_route) break;
	}//for

	return true;

}


