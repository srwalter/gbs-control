#include <Wire.h>
#include "ntsc_240p.h"
#include "pal_240p.h"
#include "ntsc_feedbackclock.h"
#include "pal_feedbackclock.h"
#include "ntsc_1280x720.h"
#include "ntsc_1280x1024.h"
#include "pal_1280x720.h"
#include "presetMdSection.h"

#include "tv5725.h"
#include "framesync.h"
#include "osd.h"

typedef TV5725<GBS_ADDR> GBS;

#if defined(ESP8266)  // select WeMos D1 R2 & mini in IDE for NodeMCU! (otherwise LED_BUILTIN is mapped to D0 / does not work)
#include <ESP8266WiFi.h>
#include "FS.h"
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "PersWiFiManager.h"
#include <ESP8266mDNS.h>  // mDNS library for finding gbscontrol.local on the local network

// WebSockets library by Markus Sattler
// to install: "Sketch" > "Include Library" > "Manage Libraries ..." > search for "websockets" and install "WebSockets for Arduino (Server + Client)"
#include <WebSocketsServer.h>

const char* ap_ssid = "gbscontrol";
const char* ap_password = "qqqqqqqq";
ESP8266WebServer server(80);
DNSServer dnsServer;
WebSocketsServer webSocket(81);
PersWiFiManager persWM(server, dnsServer);

#define DEBUG_IN_PIN D6 // marked "D12/MISO/D6" (Wemos D1) or D6 (Lolin NodeMCU)
// SCL = D1 (Lolin), D15 (Wemos D1) // ESP8266 Arduino default map: SCL
// SDA = D2 (Lolin), D14 (Wemos D1) // ESP8266 Arduino default map: SDA
#define LEDON \
  pinMode(LED_BUILTIN, OUTPUT); \
  digitalWrite(LED_BUILTIN, LOW)
#define LEDOFF \
  digitalWrite(LED_BUILTIN, HIGH); \
  pinMode(LED_BUILTIN, INPUT)

// fast ESP8266 digitalRead (21 cycles vs 77), *should* work with all possible input pins
// but only "D7" and "D6" have been tested so far
#define digitalRead(x) ((GPIO_REG_READ(GPIO_IN_ADDRESS) >> x) & 1)

#else // Arduino
#define LEDON \
  pinMode(LED_BUILTIN, OUTPUT); \
  digitalWrite(LED_BUILTIN, HIGH)
#define LEDOFF \
  digitalWrite(LED_BUILTIN, LOW); \
  pinMode(LED_BUILTIN, INPUT)

#define DEBUG_IN_PIN 11

// fastest, but non portable (Uno pin 11 = PB3, Mega2560 pin 11 = PB5)
//#define digitalRead(x) bitRead(PINB, 3)
#include "fastpin.h"
#define digitalRead(x) fastRead<x>()

//#define HAVE_BUTTONS
#define INPUT_PIN 9
#define DOWN_PIN 8
#define UP_PIN 7
#define MENU_PIN 6

#endif

//
// Sync locking tunables/magic numbers
//
struct FrameSyncAttrs {
  //static const uint8_t vsyncInPin = VSYNC_IN_PIN;
  static const uint8_t debugInPin = DEBUG_IN_PIN;
  // Sync lock sampling timeout in microseconds
  static const uint32_t timeout = 900000;
  // Sync lock interval in milliseconds
  static const uint32_t lockInterval = 60 * 16; // every 60 frames. good range for this: 30 to 90
  // Sync correction in scanlines to apply when phase lags target
  static const int16_t correction = 2;
  // Target vsync phase offset (output trails input) in degrees
  static const uint32_t targetPhase = 90;
  // Number of consistent best htotal results to get in a row before considering it valid
  // ! deprecated when switching htotal measurement source to VDS
  //static const uint8_t htotalStable = 4;
  // Number of samples to average when determining best htotal
  static const uint8_t samples = 2;
};
typedef FrameSyncManager<GBS, FrameSyncAttrs> FrameSync;

struct MenuAttrs {
  static const int8_t shiftDelta = 4;
  static const int8_t scaleDelta = 4;
  static const int16_t vertShiftRange = 300;
  static const int16_t horizShiftRange = 400;
  static const int16_t vertScaleRange = 100;
  static const int16_t horizScaleRange = 130;
  static const int16_t barLength = 100;
};
typedef MenuManager<GBS, MenuAttrs> Menu;

// runTimeOptions holds system variables
struct runTimeOptions {
  unsigned long applyPresetDoneTime;
  uint16_t sourceVLines;
  uint8_t videoStandardInput; // 0 - unknown, 1 - NTSC like, 2 - PAL like, 3 480p NTSC, 4 576p PAL
  uint8_t phaseSP;
  uint8_t phaseADC;
  uint8_t currentLevelSOG;
  uint8_t syncLockFailIgnore;
  uint8_t currentSyncProcessorMode; // SD, HD, VGA // todo: replace
  boolean inputIsYpBpR;
  boolean syncWatcherEnabled;
  boolean outModeSynclock;
  boolean printInfos;
  boolean sourceDisconnected;
  boolean webServerEnabled;
  boolean webServerStarted;
  boolean allowUpdatesOTA;
  boolean webSocketConnected;
  boolean syncLockEnabled;
  boolean deinterlacerWasTurnedOff;
  boolean clampWasTurnedOff;
  boolean applyPresetDone;
  boolean forceRetime;
} rtos;
struct runTimeOptions *rto = &rtos;

// userOptions holds user preferences / customizations
struct userOptions {
  uint8_t presetPreference; // 0 - normal, 1 - feedback clock, 2 - customized, 3 - 720p, 4 - 1280x1024
  uint8_t presetGroup;
  uint8_t enableFrameTimeLock;
  uint8_t frameTimeLockMethod;
  uint8_t enableAutoGain;
} uopts;
struct userOptions *uopt = &uopts;

char globalCommand;

#if defined(ESP8266)
// serial mirror class for websocket logs
class SerialMirror : public Stream {
  size_t write(const uint8_t *data, size_t size) {
#if defined(ESP8266)
    //if (rto->webSocketConnected) {
      webSocket.sendTXT(0, data, size);
    //}
#endif
    Serial.write(data, size);
    //Serial1.write(data, size);
    return size;
  }

  size_t write(uint8_t data) {
#if defined(ESP8266)
    //if (rto->webSocketConnected) {
      webSocket.sendTXT(0, &data);
    //}
#endif
    Serial.write(data);
    //Serial1.write(data);
    return 1;
  }

  int available() {
    return 0;
  }
  int read() {
    return -1;
  }
  int peek() {
    return -1;
  }
  void flush() {       }
};

SerialMirror SerialM;
#else
#define SerialM Serial
#endif

static uint8_t lastSegment = 0xFF;

static inline void writeOneByte(uint8_t slaveRegister, uint8_t value)
{
  writeBytes(slaveRegister, &value, 1);
}

static inline void writeBytes(uint8_t slaveRegister, uint8_t* values, uint8_t numValues)
{
  if (slaveRegister == 0xF0 && numValues == 1) {
    lastSegment = *values;
  }
  else
    GBS::write(lastSegment, slaveRegister, values, numValues);
}

void copyBank(uint8_t* bank, const uint8_t* programArray, uint16_t* index)
{
  for (uint8_t x = 0; x < 16; ++x) {
    bank[x] = pgm_read_byte(programArray + *index);
    (*index)++;
  }
}

void zeroAll()
{
  // turn processing units off first
  writeOneByte(0xF0, 0);
  writeOneByte(0x46, 0x00); // reset controls 1
  writeOneByte(0x47, 0x00); // reset controls 2

  // zero out entire register space
  for (int y = 0; y < 6; y++)
  {
    writeOneByte(0xF0, (uint8_t)y);
    for (int z = 0; z < 16; z++)
    {
      uint8_t bank[16];
      for (int w = 0; w < 16; w++)
      {
        bank[w] = 0;
      }
      writeBytes(z * 16, bank, 16);
    }
  }
}

void loadPresetMdSection() {
  uint16_t index = 0;
  uint8_t bank[16];
  writeOneByte(0xF0, 1);
  for (int j = 6; j <= 7; j++) { // start at 0x60
    copyBank(bank, presetMdSection, &index);
    writeBytes(j * 16, bank, 16);
  }
  bank[0] = pgm_read_byte(presetMdSection + index);
  bank[1] = pgm_read_byte(presetMdSection + index + 1);
  bank[2] = pgm_read_byte(presetMdSection + index + 2);
  bank[3] = pgm_read_byte(presetMdSection + index + 3);
  writeBytes(8 * 16, bank, 4); // MD section ends at 0x83, not 0x90
}

void writeProgramArrayNew(const uint8_t* programArray)
{
  uint16_t index = 0;
  uint8_t bank[16];
  uint8_t y = 0;

  // programs all valid registers (the register map has holes in it, so it's not straight forward)
  // 'index' keeps track of the current preset data location.
  
  writeOneByte(0xF0, 0);
  writeOneByte(0x46, 0x00); // reset controls 1
  writeOneByte(0x47, 0x00); // reset controls 2

  for (; y < 6; y++)
  {
    writeOneByte(0xF0, (uint8_t)y);
    switch (y) {
    case 0:
      for (int j = 0; j <= 1; j++) { // 2 times
        for (int x = 0; x <= 15; x++) {
          if (j == 0 && x == 4) {
            // keep DAC off for now
            bank[x] = pgm_read_byte(programArray + index) & ~(1 << 0);
          }
          else if (j == 0 && (x == 6 || x == 7)) {
            // keep reset controls active
            bank[x] = 0;
          }
          else {
            // use preset values
            bank[x] = pgm_read_byte(programArray + index);
          }

          index++;
        }
        writeBytes(0x40 + (j * 16), bank, 16);
      }
      copyBank(bank, programArray, &index);
      writeBytes(0x90, bank, 16);
      break;
    case 1:
      for (int j = 0; j <= 2; j++) { // 3 times
        copyBank(bank, programArray, &index);
        writeBytes(j * 16, bank, 16);
      }
      loadPresetMdSection();
      break;
    case 2:
      for (int j = 0; j <= 3; j++) { // 4 times
        copyBank(bank, programArray, &index);
        writeBytes(j * 16, bank, 16);
      }
      break;
    case 3:
      for (int j = 0; j <= 7; j++) { // 8 times
        copyBank(bank, programArray, &index);
        writeBytes(j * 16, bank, 16);
      }
      // blank out VDS PIP registers, otherwise they can end up uninitialized
      for (int x = 0; x <= 15; x++) {
        writeOneByte(0x80 + x, 0x00);
      }
      break;
    case 4:
      for (int j = 0; j <= 5; j++) { // 6 times
        copyBank(bank, programArray, &index);
        writeBytes(j * 16, bank, 16);
      }
      break;
    case 5:
      for (int j = 0; j <= 6; j++) { // 7 times
        for (int x = 0; x <= 15; x++) {
          bank[x] = pgm_read_byte(programArray + index);
          if (index == 386) { // s5_02 bit 6+7 = input selector (only bit 6 is relevant)
            if (rto->inputIsYpBpR)bitClear(bank[x], 6);
            else bitSet(bank[x], 6);
          }
          index++;
        }
        writeBytes(j * 16, bank, 16);
      }
      break;
    }
  }
}

void setResetParameters() {
  SerialM.println("<reset>");
  rto->videoStandardInput = 0;
  rto->clampWasTurnedOff = 1;
  rto->applyPresetDone = 0;
  rto->sourceVLines = 0;
  GBS::RESET_CONTROL_0x46::write(0x00); // all units off
  GBS::RESET_CONTROL_0x47::write(0x00);
  GBS::DAC_RGBS_PWDNZ::write(0); // disable DAC (output)
  GBS::PLL648_CONTROL_01::write(0x00); // VCLK(1/2/4) display clock // needs valid for debug bus
  GBS::IF_SEL_ADC_SYNC::write(1); // ! 1_28
  GBS::PLLAD_VCORST::write(1); // reset = 1
  GBS::PLL_ADS::write(1); // When = 1, input clock is from ADC ( otherwise, from unconnected clock at pin 40 )
  GBS::PLL_CKIS::write(0); // PLL use OSC clock
  GBS::PLL_MS::write(2); // fb memory clock can go lower power
  GBS::PAD_CONTROL_00_0x48::write(0x2b); //disable digital inputs, enable debug out pin
  GBS::PAD_CONTROL_01_0x49::write(0x1f); //pad control pull down/up transistors on
  GBS::INTERRUPT_CONTROL_01::write(0xff); // enable interrupts
  // adc for sog detection
  GBS::ADC_INPUT_SEL::write(1); // 1 = RGBS / RGBHV adc data input
  GBS::ADC_TR_RSEL::write(2); // 5_04 // can't put 2 because of yuv failing // update: and now?
  GBS::ADC_TEST::write(2); // 5_0c // should work now
  GBS::ADC_SOGEN::write(1);
  GBS::ADC_POWDZ::write(1); // ADC on
  GBS::PLLAD_ICP::write(1);
  GBS::PLLAD_FS::write(0); // low gain (have to deal with cold and warm startups)
  GBS::PLLAD_5_16::write(0x1f); // this maybe needs to be the sog detection default
  GBS::PLLAD_MD::write(0x700);
  resetPLL(); // cycles PLL648
  resetPLLAD(); // same for PLLAD
  GBS::PLL_VCORST::write(1); // reset on
  GBS::PLLAD_CONTROL_00_5x11::write(0x01); // reset on
  enableDebugPort();
  GBS::RESET_CONTROL_0x47::write(0x16);
  GBS::INTERRUPT_CONTROL_00::write(0xff); // reset irq status
  GBS::INTERRUPT_CONTROL_00::write(0x00);
  GBS::RESET_CONTROL_0x47::write(0x16); // decimation off
}

void setAdcParameters() {
  GBS::ADC_ROFCTRL::write(0x41);
  GBS::ADC_GOFCTRL::write(0x41);
  GBS::ADC_BOFCTRL::write(0x41);
  GBS::ADC_RGCTRL::write(0x7f);
  GBS::ADC_GGCTRL::write(0x7f);
  GBS::ADC_BGCTRL::write(0x7f);
}

void setSpParameters() {
  rto->currentSyncProcessorMode = 0;
  writeOneByte(0xF0, 5);
  GBS::SP_SOG_P_ATO::write(1); // 5_20 enable sog auto polarity, leave rest alone
  // H active detect control
  writeOneByte(0x21, 0x20); // SP_SYNC_TGL_THD    H Sync toggle times threshold  0x20 // ! lower than 5_33, 4 ticks (ie 20 < 24)  !
  writeOneByte(0x22, 0x10); // SP_L_DLT_REG       Sync pulse width different threshold (little than this as equal). // 7
  writeOneByte(0x23, 0x00); // UNDOCUMENTED       range from 0x00 to at least 0x1d
  writeOneByte(0x24, 0x0b); // SP_T_DLT_REG       H total width different threshold rgbhv: b // range from 0x02 upwards
  writeOneByte(0x25, 0x00); // SP_T_DLT_REG
  writeOneByte(0x26, 0x08); // SP_SYNC_PD_THD     H sync pulse width threshold // from 0(?) to 0x50 // in yuv 720p range only to 0x0a!
  writeOneByte(0x27, 0x00); // SP_SYNC_PD_THD
  writeOneByte(0x2a, 0x03); // SP_PRD_EQ_THD     ! How many continue legal line as valid // effect on MD recovery after sync loss
  // V active detect control
  // these 4 have no effect currently test string:  s5s2ds34 s5s2es24 s5s2fs16 s5s31s84   |   s5s2ds02 s5s2es04 s5s2fs02 s5s31s04
  writeOneByte(0x2d, 0x02); // SP_VSYNC_TGL_THD   V sync toggle times threshold // at 5 starts to drop many 0_16 vsync events
  writeOneByte(0x2e, 0x00); // SP_SYNC_WIDTH_DTHD V sync pulse width threshod
  writeOneByte(0x2f, 0x02); // SP_V_PRD_EQ_THD    How many continue legal v sync as valid // at 4 starts to drop 0_16 vsync events
  writeOneByte(0x31, 0x2f); // SP_VT_DLT_REG      V total different threshold
  // Timer value control
  writeOneByte(0x33, 0x2e); // SP_H_TIMER_VAL    ! H timer value for h detect (was 0x28) // coupled with 5_2a and 5_21 // test bus 5_63 to 0x25 and scope dbg pin
  writeOneByte(0x34, 0x05); // SP_V_TIMER_VAL     V timer for V detect // affects 0_16 vsactive
  // Sync separation control
  writeOneByte(0x35, 0x15); // SP_DLT_REG [7:0]   Sync pulse width difference threshold  (tweak point)
  writeOneByte(0x36, 0x00); // SP_DLT_REG [11:8]

  writeOneByte(0x37, 0x04); // SP_H_PULSE_IGNOR

  GBS::SP_PRE_COAST::write(0x0a);
  GBS::SP_POST_COAST::write(0x11);

  writeOneByte(0x3a, 0x03); // was 0x0a // range depends on source vtiming, from 0x03 to xxx, some good effect at lower levels

  GBS::SP_SDCS_VSST_REG_H::write(0);
  GBS::SP_SDCS_VSSP_REG_H::write(0);
  GBS::SP_SDCS_VSST_REG_L::write(3); // 5_3f
  GBS::SP_SDCS_VSSP_REG_L::write(12); // 5_40

  GBS::SP_CS_HS_ST::write(0x00);
  GBS::SP_CS_HS_SP::write(0x08); // was 0x05, 720p source needs 0x08, this is okay with other sources

  writeOneByte(0x49, 0x04); // 0x04 rgbhv: 20
  writeOneByte(0x4a, 0x00); // 0xc0
  writeOneByte(0x4b, 0x34); // 0x34 rgbhv: 50
  writeOneByte(0x4c, 0x00); // 0xc0

  writeOneByte(0x51, 0x02); // 0x00 rgbhv: 2
  writeOneByte(0x52, 0x00); // 0xc0
  writeOneByte(0x53, 0x06); // 0x05 rgbhv: 6
  writeOneByte(0x54, 0x00); // 0xc0

  writeOneByte(0x3e, 0x00); // SP sub coast on / with ofw protect disabled; snes 239 to normal rapid switches
  GBS::SP_CLAMP_MANUAL::write(1); // 1 = automatic on/off possible // todo: test this more
  GBS::SP_CLP_SRC_SEL::write(1); // clamp source 1: pixel clock, 0: 27mhz
  //GBS::SP_HS_PROC_INV_REG::write(1); // appears that SOG processing inverts hsync, this corrects it // not needed when S5_57, 6 is on
  GBS::SP_SOG_MODE::write(1);
  GBS::SP_NO_CLAMP_REG::write(1); // yuv inputs need this
  //GBS::SP_DIS_SUB_COAST::write(1);
  GBS::SP_H_CST_ST::write(30);
  GBS::SP_H_CST_SP::write(30);
  GBS::SP_HCST_AUTO_EN::write(1);
  GBS::SP_PRE_COAST::write(9);
  GBS::SP_POST_COAST::write(16);

  // these regs seem to be shifted in the docs. doc 0x58 is actually 0x59 etc
  writeOneByte(0x58, 0x05); //rgbhv: 0
  writeOneByte(0x59, 0x10); //rgbhv: 0
  writeOneByte(0x5a, 0x00); //rgbhv: 0 // was 0x05 but 480p ps2 doesnt like it
  writeOneByte(0x5b, 0x02); //rgbhv: 0
  writeOneByte(0x5c, 0x00); //rgbhv: 0
  writeOneByte(0x5d, 0x02); //rgbhv: 0
}

// Sync detect resolution: 5bits; comparator voltage range 10mv~320mv.
// -> 10mV per step; if cables and source are to standard, use 100mV (level 10)
void setAndUpdateSogLevel(uint8_t level) {
  GBS::ADC_SOGCTRL::write(level);
  rto->currentLevelSOG = level;
  //SerialM.print("sog level: "); SerialM.println(rto->currentLevelSOG);
}

void syncProcessorModeSDHD() {

}

void syncProcessorModeVGA() {
  rto->syncLockEnabled = false; // not necessary, since VDS is off / bypassed
  writeOneByte(0xF0, 5);
  writeOneByte(0x3e, 0x30);
  GBS::SP_NO_COAST_REG::write(1);
  writeOneByte(0x38, 0x00);
  writeOneByte(0x39, 0x00);
  writeOneByte(0xF0, 0);
  writeOneByte(0x40, 0x02); // source clocks from pll, 81Mhz mem clock

  rto->phaseADC = 16;
  rto->phaseSP = 18;
  setPhaseSP();
  setPhaseADC();

  rto->currentSyncProcessorMode = 3;
}

// in operation: t5t04t1 for 10% lower power on ADC
// also: s0s40s1c for 5% (lower memclock of 108mhz)
// for some reason: t0t45t2 t0t45t4 (enable SDAC, output max voltage) 5% lower  done in presets
// t0t4ft4 clock out should be off
// s4s01s20 (was 30) faster latency // unstable at 108mhz
// both phase controls off saves some power 506ma > 493ma
// oversample ratio can save 10% at 1x
// t3t24t3 VDS_TAP6_BYPS on can save 2%

// Generally, the ADC has to stay enabled to perform SOG separation and thus "see" a source.
// It is possible to run in low power mode.
void goLowPowerWithInputDetection() {
  SerialM.println("low power");
  setSpParameters();
  loadPresetMdSection(); // fills 1_60 to 1_83 (mode detect segment, mostly static)
  setAndUpdateSogLevel(rto->currentLevelSOG);
  setResetParameters();
  delay(300);
  LEDOFF;
}

void optimizeSogLevel() {
  if (rto->videoStandardInput == 15 || GBS::SP_SOG_MODE::read() != 1) return;
  uint8_t jitter = 10;
  GBS::PLLAD_PDZ::write(1);
  uint8_t debugRegSp = GBS::TEST_BUS_SP_SEL::read();
  uint8_t debugRegBus = GBS::TEST_BUS_SEL::read();
  GBS::TEST_BUS_SP_SEL::write(0x5b); // # 5 cs_sep module
  GBS::PLLAD_PDZ::write(1);
  GBS::TEST_BUS_SEL::write(0xa);

  rto->currentLevelSOG = 31;
  setAndUpdateSogLevel(rto->currentLevelSOG);
  delay(80);
  while (1) {
    jitter = 0;
    for (uint8_t i = 0; i < 10; i++) {
      uint16_t test1 = GBS::TEST_BUS::read() & 0x07ff;
      delay(random(2, 6)); // random(inclusive, exclusive));
      uint16_t test2 = GBS::TEST_BUS::read() & 0x07ff;
      if (((test1 & 0x00ff) == (test2 & 0x00ff)) && ((test1 > 0x00d0) && (test1 < 0x0180))) {
        jitter++;
        //Serial.print("1: ");Serial.print(test1, HEX);Serial.print(" 2: ");Serial.println(test2, HEX);
      }
    }
    if (jitter >= 10) { break; } // found
    SerialM.print("+");
    setAndUpdateSogLevel(rto->currentLevelSOG);
    delay(10);
    if (rto->currentLevelSOG >= 1) rto->currentLevelSOG -= 1;
    else {
      break;
    }
  }

  if (rto->currentLevelSOG < 31 && rto->currentLevelSOG > 16) { rto->currentLevelSOG = 9; }
  else if (rto->currentLevelSOG >= 12) { rto->currentLevelSOG -= 7; }
  else if (rto->currentLevelSOG >= 3) { rto->currentLevelSOG -= 2; }
  SerialM.print("\nsog level: "); SerialM.println(rto->currentLevelSOG);
  setAndUpdateSogLevel(rto->currentLevelSOG);

  GBS::TEST_BUS_SP_SEL::write(debugRegSp);
  GBS::TEST_BUS_SEL::write(debugRegBus);
}

// GBS boards have 2 potential sync sources:
// - RCA connectors
// - VGA input / 5 pin RGBS header / 8 pin VGA header (all 3 are shared electrically)
// This routine looks for sync on the currently active input. If it finds it, the input is returned.
// If it doesn't find sync, it switches the input and returns 0, so that an active input will be found eventually.
// This is done this way to not block the control MCU with active searching for sync.
uint8_t detectAndSwitchToActiveInput() { // if any
  uint8_t readout = 0;
  static boolean toggle = 0;
  static uint8_t rgbhvCheck = 0;
  uint8_t currentInput = GBS::ADC_INPUT_SEL::read();

  if (currentInput == 1) {
    rgbhvCheck++;
    if (rgbhvCheck == 3) { // skip on first run (favor RGBS a little over RGBHV)
      rgbhvCheck = 0;
      GBS::SP_EXT_SYNC_SEL::write(0); // connect HV input
      GBS::SP_SOG_MODE::write(0);
      GBS::ADC_SOGEN::write(0);
      writeOneByte(0xF0, 0);
      unsigned long timeOutStart = millis();
      uint8_t vsyncActive = 0;
      while (!vsyncActive && millis() - timeOutStart < 500) {
        readFromRegister(0x16, 1, &readout);
        vsyncActive = readout & 0x08;
        delay(1); // wifi stack
      }
      if (vsyncActive) {
        SerialM.println("RGBHV");
        rto->inputIsYpBpR = 0;
        rto->sourceDisconnected = false;
        applyRGBPatches();
        GBS::SP_EXT_SYNC_SEL::write(0); // connect HV input
        GBS::ADC_SOGEN::write(0);
        GBS::SP_SOG_MODE::write(0);
        rto->videoStandardInput = 15;
        disableVDS();
        applyPresets(rto->videoStandardInput); // exception for this mode: apply it right here, not later in syncwatcher
        LEDON;
        return 3;
      }
      else {
        GBS::SP_EXT_SYNC_SEL::write(1);
        GBS::SP_SOG_MODE::write(1);
        GBS::ADC_SOGEN::write(1);
      }
    }
  }

  unsigned long timeout = millis();
  while (readout == 0 && millis() - timeout < 150) {
    readout = GBS::TEST_BUS_2F::read();
    uint8_t videoMode = getVideoMode();
    if (readout != 0 || videoMode > 0) {
      SerialM.print("found: "); SerialM.print(readout); SerialM.print(" getVideoMode: "); SerialM.print(getVideoMode());
      SerialM.print(" input: "); SerialM.print(currentInput);
      if (currentInput == 1) { // RGBS
        SerialM.println(" RGBS");
        resetPLLAD();
        unsigned long timeOutStart = millis();
        unsigned long now = millis();
        while (!getVideoMode() && (now - timeOutStart < 1000)) {
          // try finding the signal
          GBS::SP_NO_CLAMP_REG::write(1); rto->clampWasTurnedOff = 1;
          delay(40);
          if (getVideoMode() > 0) { break; }
          else { now = millis(); }
        }
        if (millis() - timeOutStart >= 1000) {
          SerialM.print("\n");
          return 0;
        }
        rto->inputIsYpBpR = 0;
        rto->sourceDisconnected = false;
        LEDON;
        return 1;
      }
      else if (currentInput == 0) { // YUV
        SerialM.println(" YUV");
        resetPLLAD();
        rto->inputIsYpBpR = 1;
        rto->sourceDisconnected = false;
        if (GBS::SP_NO_CLAMP_REG::read() == 0) { SerialM.println("!!A"); } // assert: must be 1
        LEDON;
        unsigned long timeOutStart = millis();
        unsigned long now = millis();
        while (!getVideoMode() && (now - timeOutStart < 1000)) {
          // try finding the signal
          //GBS::SP_NO_CLAMP_REG::write(1); rto->clampWasTurnedOff = 1;
          delay(40);
          if (getVideoMode() > 0) { break; }
          else { now = millis(); }
        }
        return 2;
      }
    }
    delay(1); // wifi stack
  }

  GBS::ADC_INPUT_SEL::write(toggle); // RGBS test
 // resetModeDetect(); //sure?
  toggle = !toggle;

  return 0;
}

void inputAndSyncDetect() {
  setAndUpdateSogLevel(rto->currentLevelSOG);
  uint8_t syncFound = detectAndSwitchToActiveInput();

  if (syncFound == 0) {
    SerialM.println("no sync found");
    if (!getSyncPresent()) {
      rto->sourceDisconnected = true;
      rto->videoStandardInput = 0;
      // reset to base settings, then go to low power
      GBS::SP_EXT_SYNC_SEL::write(1); // disconnect HV input
      GBS::ADC_SOGEN::write(1);
      GBS::SP_SOG_MODE::write(1);
      goLowPowerWithInputDetection();
    }
  }
  else if (syncFound == 1) { // input is RGBS
    GBS::SP_EXT_SYNC_SEL::write(1); // disconnect HV input
    rto->sourceDisconnected = false;
    applyRGBPatches();
  }
  else if (syncFound == 2) {
    GBS::SP_EXT_SYNC_SEL::write(1); // disconnect HV input
    rto->sourceDisconnected = false;
    applyYuvPatches();
  }
  else if (syncFound == 3) { // input is RGBHV
    //already applied
  }
}

uint8_t getSingleByteFromPreset(const uint8_t* programArray, unsigned int offset) {
  return pgm_read_byte(programArray + offset);
}

static inline void readFromRegister(uint8_t reg, int bytesToRead, uint8_t* output)
{
  return GBS::read(lastSegment, reg, output, bytesToRead);
}

void printReg(uint8_t seg, uint8_t reg) {
  uint8_t readout;
  readFromRegister(reg, 1, &readout);
  // didn't think this HEX trick would work, but it does! (?)
  SerialM.print("0x"); SerialM.print(readout, HEX); SerialM.print(", // s"); SerialM.print(seg); SerialM.print("_"); SerialM.println(reg, HEX);
}

// dumps the current chip configuration in a format that's ready to use as new preset :)
void dumpRegisters(byte segment)
{
  if (segment > 5) return;
  writeOneByte(0xF0, segment);

  switch (segment) {
  case 0:
    for (int x = 0x40; x <= 0x5F; x++) {
      printReg(0, x);
    }
    for (int x = 0x90; x <= 0x9F; x++) {
      printReg(0, x);
    }
    break;
  case 1:
    for (int x = 0x0; x <= 0x2F; x++) {
      printReg(1, x);
    }
    break;
  case 2:
    for (int x = 0x0; x <= 0x3F; x++) {
      printReg(2, x);
    }
    break;
  case 3:
    for (int x = 0x0; x <= 0x7F; x++) {
      printReg(3, x);
    }
    break;
  case 4:
    for (int x = 0x0; x <= 0x5F; x++) {
      printReg(4, x);
    }
    break;
  case 5:
    for (int x = 0x0; x <= 0x6F; x++) {
      printReg(5, x);
    }
    break;
  }
}

void resetPLLAD() {
  GBS::PLLAD_VCORST::write(1);
  delay(1);
  GBS::PLLAD_VCORST::write(0);
  delay(1);
  latchPLLAD();
}

void latchPLLAD() {
  GBS::PLLAD_LAT::write(0);
  delay(1);
  GBS::PLLAD_LAT::write(1);
}

void resetPLL() {
  GBS::PAD_OSC_CNTRL::write(2); // crystal drive
  GBS::PLL_LEN::write(0);
  GBS::PLL_VCORST::write(1);
  delay(1);
  GBS::PLL_VCORST::write(0);
  delay(1);
  GBS::PLL_LEN::write(1);
}

void ResetSDRAM() {
  GBS::SDRAM_RESET_CONTROL::write(0x07); delay(2); // enable "Software Control SDRAM Idle Period" 0x00 for off
  GBS::SDRAM_RESET_SIGNAL::write(1); //delay(4);
  GBS::SDRAM_START_INITIAL_CYCLE::write(1); delay(4);
  GBS::SDRAM_RESET_SIGNAL::write(0); //delay(2);
  GBS::SDRAM_START_INITIAL_CYCLE::write(0); //delay(2);
}

// soft reset cycle
// This restarts all chip units, which is sometimes required when important config bits are changed.
void resetDigital() {
  if (GBS::SFTRST_VDS_RSTZ::read() == 1) { // if VDS enabled
    GBS::RESET_CONTROL_0x46::write(0x40); // then keep it enabled
  }
  else {
    GBS::RESET_CONTROL_0x46::write(0x00);
  }
  GBS::RESET_CONTROL_0x47::write(0x00);
  // enable
  if (rto->videoStandardInput != 15) GBS::RESET_CONTROL_0x46::write(0x7f);
  GBS::RESET_CONTROL_0x47::write(0x17);  // all on except HD bypass
  delay(1);
  ResetSDRAM();
  resetPLL();
}

void SyncProcessorOffOn() {
  disableDeinterlacer();
  GBS::SFTRST_SYNC_RSTZ::write(0);
  delay(1);
  GBS::SFTRST_SYNC_RSTZ::write(1);
  delay(220); enableDeinterlacer();
}

void resetModeDetect() {
  GBS::SFTRST_MODE_RSTZ::write(0);
  delay(1); // needed
  GBS::SFTRST_MODE_RSTZ::write(1);
}

void shiftHorizontal(uint16_t amountToAdd, bool subtracting) {
  typedef GBS::Tie<GBS::VDS_HB_ST, GBS::VDS_HB_SP> Regs;
  uint16_t hrst = GBS::VDS_HSYNC_RST::read();
  uint16_t hbst = 0, hbsp = 0;

  Regs::read(hbst, hbsp);

  // Perform the addition/subtraction
  if (subtracting) {
    hbst -= amountToAdd;
    hbsp -= amountToAdd;
  }
  else {
    hbst += amountToAdd;
    hbsp += amountToAdd;
  }

  // handle the case where hbst or hbsp have been decremented below 0
  if (hbst & 0x8000) {
    hbst = hrst % 2 == 1 ? (hrst + hbst) + 1 : (hrst + hbst);
  }
  if (hbsp & 0x8000) {
    hbsp = hrst % 2 == 1 ? (hrst + hbsp) + 1 : (hrst + hbsp);
  }

  // handle the case where hbst or hbsp have been incremented above hrst
  if (hbst > hrst) {
    hbst = hrst % 2 == 1 ? (hbst - hrst) - 1 : (hbst - hrst);
  }
  if (hbsp > hrst) {
    hbsp = hrst % 2 == 1 ? (hbsp - hrst) - 1 : (hbsp - hrst);
  }

  Regs::write(hbst, hbsp);
}

void shiftHorizontalLeft() {
  shiftHorizontal(4, true);
}

void shiftHorizontalRight() {
  shiftHorizontal(4, false);
}

void shiftHorizontalLeftIF(uint8_t amount) {
  uint16_t IF_HB_ST2 = GBS::IF_HB_ST2::read() + amount;
  uint16_t IF_HB_SP2 = GBS::IF_HB_SP2::read() + amount;
  if (rto->videoStandardInput <= 2) {
    GBS::IF_HSYNC_RST::write(GBS::PLLAD_MD::read() / 2); // input line length from pll div
  }
  else if (rto->videoStandardInput <= 6) {
    GBS::IF_HSYNC_RST::write(GBS::PLLAD_MD::read());
  }
  uint16_t IF_HSYNC_RST = GBS::IF_HSYNC_RST::read();

  GBS::IF_LINE_SP::write(IF_HSYNC_RST + 1);

  // start
  if (IF_HB_ST2 < IF_HSYNC_RST) GBS::IF_HB_ST2::write(IF_HB_ST2);
  else {
    GBS::IF_HB_ST2::write(IF_HB_ST2 - IF_HSYNC_RST);
  }
  //SerialM.print("IF_HB_ST2:  "); SerialM.println(GBS::IF_HB_ST2::read());

  // stop
  if (IF_HB_SP2 < IF_HSYNC_RST) GBS::IF_HB_SP2::write(IF_HB_SP2);
  else {
    GBS::IF_HB_SP2::write((IF_HB_SP2 - IF_HSYNC_RST) + 1);
  }
  //SerialM.print("IF_HB_SP2:  "); SerialM.println(GBS::IF_HB_SP2::read());
}

void shiftHorizontalRightIF(uint8_t amount) {
  int16_t IF_HB_ST2 = GBS::IF_HB_ST2::read() - amount;
  int16_t IF_HB_SP2 = GBS::IF_HB_SP2::read() - amount;
  if (rto->videoStandardInput <= 2) {
    GBS::IF_HSYNC_RST::write(GBS::PLLAD_MD::read() / 2); // input line length from pll div
  }
  else if (rto->videoStandardInput <= 6) {
    GBS::IF_HSYNC_RST::write(GBS::PLLAD_MD::read());
  }
  int16_t IF_HSYNC_RST = GBS::IF_HSYNC_RST::read();

  GBS::IF_LINE_SP::write(IF_HSYNC_RST + 1);

  if (IF_HB_ST2 > 0) GBS::IF_HB_ST2::write(IF_HB_ST2);
  else {
    GBS::IF_HB_ST2::write(IF_HSYNC_RST - 1);
  }
  //SerialM.print("IF_HB_ST2:  "); SerialM.println(GBS::IF_HB_ST2::read());

  if (IF_HB_SP2 > 0) GBS::IF_HB_SP2::write(IF_HB_SP2);
  else {
    GBS::IF_HB_SP2::write(IF_HSYNC_RST - 1);
    //GBS::IF_LINE_SP::write(GBS::IF_LINE_SP::read() - 2);
  }
  //SerialM.print("IF_HB_SP2:  "); SerialM.println(GBS::IF_HB_SP2::read());
}

void scaleHorizontal(uint16_t amountToAdd, bool subtracting) {
  uint16_t hscale = GBS::VDS_HSCALE::read();

  if (subtracting && (hscale - amountToAdd > 0)) {
    hscale -= amountToAdd;
  }
  else if (hscale + amountToAdd <= 1023) {
    hscale += amountToAdd;
  }

  SerialM.print("Scale Hor: "); SerialM.println(hscale);
  GBS::VDS_HSCALE::write(hscale);
}

void scaleHorizontalSmaller() {
  scaleHorizontal(1, false);
}

void scaleHorizontalLarger() {
  scaleHorizontal(1, true);
}

void moveHS(uint16_t amountToAdd, bool subtracting) {
  uint8_t high, low;
  uint16_t newST, newSP;

  writeOneByte(0xf0, 3);
  readFromRegister(0x0a, 1, &low);
  readFromRegister(0x0b, 1, &high);
  newST = ((((uint16_t)high) & 0x000f) << 8) | (uint16_t)low;
  readFromRegister(0x0b, 1, &low);
  readFromRegister(0x0c, 1, &high);
  newSP = ((((uint16_t)high) & 0x00ff) << 4) | ((((uint16_t)low) & 0x00f0) >> 4);

  if (subtracting) {
    newST -= amountToAdd;
    newSP -= amountToAdd;
  }
  else {
    newST += amountToAdd;
    newSP += amountToAdd;
  }
  //SerialM.print("HSST: "); SerialM.print(newST);
  //SerialM.print(" HSSP: "); SerialM.println(newSP);

  writeOneByte(0x0a, (uint8_t)(newST & 0x00ff));
  writeOneByte(0x0b, ((uint8_t)(newSP & 0x000f) << 4) | ((uint8_t)((newST & 0x0f00) >> 8)));
  writeOneByte(0x0c, (uint8_t)((newSP & 0x0ff0) >> 4));
}

void moveVS(uint16_t amountToAdd, bool subtracting) {
  uint16_t vtotal = GBS::VDS_VSYNC_RST::read();
  uint16_t VDS_DIS_VB_ST = GBS::VDS_DIS_VB_ST::read();
  uint16_t newVDS_VS_ST = GBS::VDS_VS_ST::read();
  uint16_t newVDS_VS_SP = GBS::VDS_VS_SP::read();

  if (subtracting) {
    if ((newVDS_VS_ST - amountToAdd) > VDS_DIS_VB_ST) {
      newVDS_VS_ST -= amountToAdd;
      newVDS_VS_SP -= amountToAdd;
    }
    else SerialM.println("limit");
  }
  else {
    if ((newVDS_VS_SP + amountToAdd) < vtotal) {
      newVDS_VS_ST += amountToAdd;
      newVDS_VS_SP += amountToAdd;
    }
    else SerialM.println("limit");
  }
  //SerialM.print("VSST: "); SerialM.print(newVDS_VS_ST);
  //SerialM.print(" VSSP: "); SerialM.println(newVDS_VS_SP);

  GBS::VDS_VS_ST::write(newVDS_VS_ST);
  GBS::VDS_VS_SP::write(newVDS_VS_SP);
}

void invertHS() {
  uint8_t high, low;
  uint16_t newST, newSP;

  writeOneByte(0xf0, 3);
  readFromRegister(0x0a, 1, &low);
  readFromRegister(0x0b, 1, &high);
  newST = ((((uint16_t)high) & 0x000f) << 8) | (uint16_t)low;
  readFromRegister(0x0b, 1, &low);
  readFromRegister(0x0c, 1, &high);
  newSP = ((((uint16_t)high) & 0x00ff) << 4) | ((((uint16_t)low) & 0x00f0) >> 4);

  uint16_t temp = newST;
  newST = newSP;
  newSP = temp;

  writeOneByte(0x0a, (uint8_t)(newST & 0x00ff));
  writeOneByte(0x0b, ((uint8_t)(newSP & 0x000f) << 4) | ((uint8_t)((newST & 0x0f00) >> 8)));
  writeOneByte(0x0c, (uint8_t)((newSP & 0x0ff0) >> 4));
}

void invertVS() {
  uint8_t high, low;
  uint16_t newST, newSP;

  writeOneByte(0xf0, 3);
  readFromRegister(0x0d, 1, &low);
  readFromRegister(0x0e, 1, &high);
  newST = ((((uint16_t)high) & 0x000f) << 8) | (uint16_t)low;
  readFromRegister(0x0e, 1, &low);
  readFromRegister(0x0f, 1, &high);
  newSP = ((((uint16_t)high) & 0x00ff) << 4) | ((((uint16_t)low) & 0x00f0) >> 4);

  uint16_t temp = newST;
  newST = newSP;
  newSP = temp;

  writeOneByte(0x0d, (uint8_t)(newST & 0x00ff));
  writeOneByte(0x0e, ((uint8_t)(newSP & 0x000f) << 4) | ((uint8_t)((newST & 0x0f00) >> 8)));
  writeOneByte(0x0f, (uint8_t)((newSP & 0x0ff0) >> 4));
}

void scaleVertical(uint16_t amountToAdd, bool subtracting) {
  uint16_t vscale = GBS::VDS_VSCALE::read();

  if (subtracting && (vscale - amountToAdd > 0)) {
    vscale -= amountToAdd;
  }
  else if (vscale + amountToAdd <= 1023) {
    vscale += amountToAdd;
  }

  SerialM.print("Scale Vert: "); SerialM.println(vscale);
  GBS::VDS_VSCALE::write(vscale);
}

void shiftVertical(uint16_t amountToAdd, bool subtracting) {
  typedef GBS::Tie<GBS::VDS_VB_ST, GBS::VDS_VB_SP> Regs;
  uint16_t vrst = GBS::VDS_VSYNC_RST::read() - FrameSync::getSyncLastCorrection();
  uint16_t vbst = 0, vbsp = 0;
  int16_t newVbst = 0, newVbsp = 0;

  Regs::read(vbst, vbsp);
  newVbst = vbst; newVbsp = vbsp;

  if (subtracting) {
    newVbst -= amountToAdd;
    newVbsp -= amountToAdd;
  }
  else {
    newVbst += amountToAdd;
    newVbsp += amountToAdd;
  }

  // handle the case where hbst or hbsp have been decremented below 0
  if (newVbst < 0) {
    newVbst = vrst + newVbst;
  }
  if (newVbsp < 0) {
    newVbsp = vrst + newVbsp;
  }

  // handle the case where vbst or vbsp have been incremented above vrstValue
  if (newVbst > (int16_t)vrst) {
    newVbst = newVbst - vrst;
  }
  if (newVbsp > (int16_t)vrst) {
    newVbsp = newVbsp - vrst;
  }

  Regs::write(newVbst, newVbsp);
  //SerialM.print("VSST: "); SerialM.print(newVbst); SerialM.print(" VSSP: "); SerialM.println(newVbsp);
}

void shiftVerticalUp() {
  shiftVertical(1, true);
}

void shiftVerticalDown() {
  shiftVertical(1, false);
}

void shiftVerticalUpIF() {
  // -4 to allow variance in source line count
  uint16_t sourceLines = GBS::VPERIOD_IF::read() - 4;
  int16_t stop = GBS::IF_VB_SP::read();
  int16_t start = GBS::IF_VB_ST::read();

  if (stop + 1 <= (int16_t)sourceLines) stop += 1;
  else stop = 0 + 1;

  if (start + 2 <= (int16_t)sourceLines) start += 1;
  else start = 0 + 1;

  GBS::IF_VB_SP::write(stop);
  GBS::IF_VB_ST::write(start);
}

void shiftVerticalDownIF() {
  uint16_t sourceLines = GBS::VPERIOD_IF::read() - 4;
  int16_t stop = GBS::IF_VB_SP::read();
  int16_t start = GBS::IF_VB_ST::read();

  if (stop - 1 >= 0) stop -= 1;
  else stop = sourceLines - 1;

  if (start - 1 >= 0) start -= 1;
  else start = sourceLines - 1;

  GBS::IF_VB_SP::write(stop);
  GBS::IF_VB_ST::write(start);
}

void setHSyncStartPosition(uint16_t value) {
  GBS::VDS_HS_ST::write(value);
}

void setHSyncStopPosition(uint16_t value) {
  GBS::VDS_HS_SP::write(value);
}

void setMemoryHblankStartPosition(uint16_t value) {
  GBS::VDS_HB_ST::write(value);
}

void setMemoryHblankStopPosition(uint16_t value) {
  GBS::VDS_HB_SP::write(value);
}

void setDisplayHblankStartPosition(uint16_t value) {
  GBS::VDS_DIS_HB_ST::write(value);
}

void setDisplayHblankStopPosition(uint16_t value) {
  GBS::VDS_DIS_HB_SP::write(value);
}

void setVSyncStartPosition(uint16_t value) {
  GBS::VDS_VS_ST::write(value);
}

void setVSyncStopPosition(uint16_t value) {
  GBS::VDS_VS_SP::write(value);
}

void setMemoryVblankStartPosition(uint16_t value) {
  GBS::VDS_VB_ST::write(value);
}

void setMemoryVblankStopPosition(uint16_t value) {
  GBS::VDS_VB_SP::write(value);
}

void setDisplayVblankStartPosition(uint16_t value) {
  GBS::VDS_DIS_VB_ST::write(value);
}

void setDisplayVblankStopPosition(uint16_t value) {
  GBS::VDS_DIS_VB_SP::write(value);
}

#if defined(ESP8266) // Arduino space saving
void getVideoTimings() {
  uint8_t  regLow;
  uint8_t  regHigh;

  uint16_t Vds_hsync_rst;
  uint16_t Vds_vsync_rst;
  uint16_t vds_dis_hb_st;
  uint16_t vds_dis_hb_sp;
  uint16_t VDS_HS_ST;
  uint16_t VDS_HS_SP;
  uint16_t VDS_DIS_VB_ST;
  uint16_t VDS_DIS_VB_SP;
  uint16_t VDS_DIS_VS_ST;
  uint16_t VDS_DIS_VS_SP;

  // get HRST
  writeOneByte(0xF0, 3);
  readFromRegister(0x01, 1, &regLow);
  readFromRegister(0x02, 1, &regHigh);
  Vds_hsync_rst = (((((uint16_t)regHigh) & 0x000f) << 8) | (uint16_t)regLow);
  SerialM.print("htotal: "); SerialM.println(Vds_hsync_rst);

  // get HS_ST
  readFromRegister(0x0a, 1, &regLow);
  readFromRegister(0x0b, 1, &regHigh);
  VDS_HS_ST = (((((uint16_t)regHigh) & 0x000f) << 8) | (uint16_t)regLow);
  SerialM.print("HS ST: "); SerialM.println(VDS_HS_ST);

  // get HS_SP
  readFromRegister(0x0b, 1, &regLow);
  readFromRegister(0x0c, 1, &regHigh);
  VDS_HS_SP = ((((uint16_t)regHigh) << 4) | ((uint16_t)regLow & 0x00f0) >> 4);
  SerialM.print("HS SP: "); SerialM.println(VDS_HS_SP);

  // get HBST
  readFromRegister(0x10, 1, &regLow);
  readFromRegister(0x11, 1, &regHigh);
  vds_dis_hb_st = (((((uint16_t)regHigh) & 0x000f) << 8) | (uint16_t)regLow);
  SerialM.print("HB ST (display): "); SerialM.println(vds_dis_hb_st);

  // get HBSP
  readFromRegister(0x11, 1, &regLow);
  readFromRegister(0x12, 1, &regHigh);
  vds_dis_hb_sp = ((((uint16_t)regHigh) << 4) | ((uint16_t)regLow & 0x00f0) >> 4);
  SerialM.print("HB SP (display): "); SerialM.println(vds_dis_hb_sp);

  // get HBST(memory)
  readFromRegister(0x04, 1, &regLow);
  readFromRegister(0x05, 1, &regHigh);
  vds_dis_hb_st = (((((uint16_t)regHigh) & 0x000f) << 8) | (uint16_t)regLow);
  SerialM.print("HB ST (memory): "); SerialM.println(vds_dis_hb_st);

  // get HBSP(memory)
  readFromRegister(0x05, 1, &regLow);
  readFromRegister(0x06, 1, &regHigh);
  vds_dis_hb_sp = ((((uint16_t)regHigh) << 4) | ((uint16_t)regLow & 0x00f0) >> 4);
  SerialM.print("HB SP (memory): "); SerialM.println(vds_dis_hb_sp);

  SerialM.println("----");
  // get VRST
  readFromRegister(0x02, 1, &regLow);
  readFromRegister(0x03, 1, &regHigh);
  Vds_vsync_rst = ((((uint16_t)regHigh) & 0x007f) << 4) | ((((uint16_t)regLow) & 0x00f0) >> 4);
  SerialM.print("vtotal: "); SerialM.println(Vds_vsync_rst);

  // get V Sync Start
  readFromRegister(0x0d, 1, &regLow);
  readFromRegister(0x0e, 1, &regHigh);
  VDS_DIS_VS_ST = (((uint16_t)regHigh & 0x0007) << 8) | ((uint16_t)regLow);
  SerialM.print("VS ST: "); SerialM.println(VDS_DIS_VS_ST);

  // get V Sync Stop
  readFromRegister(0x0e, 1, &regLow);
  readFromRegister(0x0f, 1, &regHigh);
  VDS_DIS_VS_SP = ((((uint16_t)regHigh & 0x007f) << 4) | ((uint16_t)regLow & 0x00f0) >> 4);
  SerialM.print("VS SP: "); SerialM.println(VDS_DIS_VS_SP);

  // get VBST
  readFromRegister(0x13, 1, &regLow);
  readFromRegister(0x14, 1, &regHigh);
  VDS_DIS_VB_ST = (((uint16_t)regHigh & 0x0007) << 8) | ((uint16_t)regLow);
  SerialM.print("VB ST (display): "); SerialM.println(VDS_DIS_VB_ST);

  // get VBSP
  readFromRegister(0x14, 1, &regLow);
  readFromRegister(0x15, 1, &regHigh);
  VDS_DIS_VB_SP = ((((uint16_t)regHigh & 0x007f) << 4) | ((uint16_t)regLow & 0x00f0) >> 4);
  SerialM.print("VB SP (display): "); SerialM.println(VDS_DIS_VB_SP);

  // get VBST (memory)
  readFromRegister(0x07, 1, &regLow);
  readFromRegister(0x08, 1, &regHigh);
  VDS_DIS_VB_ST = (((uint16_t)regHigh & 0x0007) << 8) | ((uint16_t)regLow);
  SerialM.print("VB ST (memory): "); SerialM.println(VDS_DIS_VB_ST);

  // get VBSP (memory)
  readFromRegister(0x08, 1, &regLow);
  readFromRegister(0x09, 1, &regHigh);
  VDS_DIS_VB_SP = ((((uint16_t)regHigh & 0x007f) << 4) | ((uint16_t)regLow & 0x00f0) >> 4);
  SerialM.print("VB SP (memory): "); SerialM.println(VDS_DIS_VB_SP);
}
#endif

void set_htotal(uint16_t htotal) {
  // ModeLine "1280x960" 108.00 1280 1376 1488 1800 960 961 964 1000 +HSync +VSync
  // front porch: H2 - H1: 1376 - 1280
  // back porch : H4 - H3: 1800 - 1488
  // sync pulse : H3 - H2: 1488 - 1376

  uint16_t h_blank_display_start_position = htotal - 1;
  uint16_t h_blank_display_stop_position = htotal - ((htotal * 3) / 4);
  uint16_t center_blank = ((h_blank_display_stop_position / 2) * 3) / 4; // a bit to the left
  uint16_t h_sync_start_position = center_blank - (center_blank / 2);
  uint16_t h_sync_stop_position = center_blank + (center_blank / 2);
  uint16_t h_blank_memory_start_position = h_blank_display_start_position - 1;
  uint16_t h_blank_memory_stop_position = h_blank_display_stop_position - (h_blank_display_stop_position / 50);

  GBS::VDS_HSYNC_RST::write(htotal);
  GBS::VDS_HS_ST::write(h_sync_start_position);
  GBS::VDS_HS_SP::write(h_sync_stop_position);
  GBS::VDS_DIS_HB_ST::write(h_blank_display_start_position);
  GBS::VDS_DIS_HB_SP::write(h_blank_display_stop_position);
  GBS::VDS_HB_ST::write(h_blank_memory_start_position);
  GBS::VDS_HB_SP::write(h_blank_memory_stop_position);
}

void set_vtotal(uint16_t vtotal) {
  uint8_t regLow, regHigh;
  // ModeLine "1280x960" 108.00 1280 1376 1488 1800 960 961 964 1000 +HSync +VSync
  // front porch: V2 - V1: 961 - 960 = 1
  // back porch : V4 - V3: 1000 - 964 = 36
  // sync pulse : V3 - V2: 964 - 961 = 3
  // VB start: 960 / 1000 = (24/25)
  // VB stop:  1000        = vtotal
  // VS start: 961 / 1000 = (961/1000)
  // VS stop : 964 / 1000 = (241/250)

  // VS stop - VB start must stay constant to avoid vertical wiggle
  // VS stop - VS start must stay constant to maintain sync
  uint16_t v_blank_start_position = ((uint32_t)vtotal * 24) / 25;
  uint16_t v_blank_stop_position = vtotal;
  // Offset by maxCorrection to prevent front porch from going negative
  uint16_t v_sync_start_position = ((uint32_t)vtotal * 961) / 1000;
  uint16_t v_sync_stop_position = ((uint32_t)vtotal * 241) / 250;

  // write vtotal
  writeOneByte(0xF0, 3);
  regHigh = (uint8_t)(vtotal >> 4);
  readFromRegister(0x02, 1, &regLow);
  regLow = ((regLow & 0x0f) | (uint8_t)(vtotal << 4));
  writeOneByte(0x03, regHigh);
  writeOneByte(0x02, regLow);

  // NTSC 60Hz: 60 kHz ModeLine "1280x960" 108.00 1280 1376 1488 1800 960 961 964 1000 +HSync +VSync
  // V-Front Porch: 961-960 = 1  = 0.1% of vtotal. Start at v_blank_start_position = vtotal - (vtotal*0.04) = 960
  // V-Back Porch:  1000-964 = 36 = 3.6% of htotal (black top lines)
  // -> vbi = 3.7 % of vtotal | almost all on top (> of 0 (vtotal+1 = 0. It wraps.))
  // vblank interval PAL would be more

  regLow = (uint8_t)v_sync_start_position;
  regHigh = (uint8_t)((v_sync_start_position & 0x0700) >> 8);
  writeOneByte(0x0d, regLow); // vs mixed
  writeOneByte(0x0e, regHigh); // vs stop
  readFromRegister(0x0e, 1, &regLow);
  readFromRegister(0x0f, 1, &regHigh);
  regLow = regLow | (uint8_t)(v_sync_stop_position << 4);
  regHigh = (uint8_t)(v_sync_stop_position >> 4);
  writeOneByte(0x0e, regLow); // vs mixed
  writeOneByte(0x0f, regHigh); // vs stop

  // VB ST
  regLow = (uint8_t)v_blank_start_position;
  readFromRegister(0x14, 1, &regHigh);
  regHigh = (uint8_t)((regHigh & 0xf8) | (uint8_t)((v_blank_start_position & 0x0700) >> 8));
  writeOneByte(0x13, regLow);
  writeOneByte(0x14, regHigh);
  //VB SP
  regHigh = (uint8_t)(v_blank_stop_position >> 4);
  readFromRegister(0x14, 1, &regLow);
  regLow = ((regLow & 0x0f) | (uint8_t)(v_blank_stop_position << 4));
  writeOneByte(0x15, regHigh);
  writeOneByte(0x14, regLow);
}

void enableDebugPort() {
  GBS::PAD_BOUT_EN::write(1); // output to pad enabled
  GBS::TEST_BUS_SEL::write(0xa); // test bus to SP
  GBS::TEST_BUS_EN::write(1);
  GBS::TEST_BUS_SP_SEL::write(0x0f); // SP test signal select (vsync in, after SOG separation)
  GBS::VDS_TEST_EN::write(1); // VDS test enable
}

void applyBestHTotal(uint16_t bestHTotal) {
  uint16_t orig_htotal = GBS::VDS_HSYNC_RST::read();
  int diffHTotal = bestHTotal - orig_htotal;
  uint16_t diffHTotalUnsigned = abs(diffHTotal);
  boolean isLargeDiff = (diffHTotalUnsigned * 10) > orig_htotal ? true : false;
  boolean requiresScalingCorrection = GBS::VDS_HSCALE::read() < 512; // output distorts if less than 512 but can be corrected
  boolean syncIsNegative = GBS::VDS_HS_ST::read() > GBS::VDS_HS_SP::read(); // if HSST > HSSP then this preset was made to have neg. active HS (ie 640x480)

  // apply correction, but only if source is different than presets timings, ie: not a custom preset or source doesn't fit custom preset
  // rto->forceRetime = true means the correction should be forced (command '.')
  // timings checked against multi clock snes
  if (((diffHTotal != 0 || requiresScalingCorrection) || rto->forceRetime == true) && bestHTotal > 400) {
    rto->forceRetime = false;
    uint16_t h_blank_display_start_position = bestHTotal; //(bestHTotal - 1); // & 0xfffe;
    uint16_t h_blank_display_stop_position = GBS::VDS_DIS_HB_SP::read() + diffHTotal;
    if (isLargeDiff) {
      h_blank_display_stop_position = h_blank_display_start_position / 5; // 1/5th of screen for blanking
    }

    uint16_t h_sync_start_position = bestHTotal >> 5;
    uint16_t h_sync_stop_position = bestHTotal >> 4;
    uint16_t h_blank_memory_start_position = h_blank_display_start_position - (h_blank_display_start_position / 48); // at least h_blank_display_start_position - 1
    uint16_t h_blank_memory_stop_position = GBS::VDS_HB_SP::read() + diffHTotal; // have to rely on currently loaded preset
    if (isLargeDiff) {
      // S4 test bus may have artifact detection!
      // probably better to hor. zoom instead of memory blank pos change
      h_blank_memory_stop_position = (h_blank_display_stop_position / 2) - (h_blank_display_stop_position / 5);
      if (diffHTotal > 0) h_blank_memory_stop_position = h_blank_display_stop_position + (h_blank_display_stop_position / 6);
      SerialM.println("large diff!");
    }

    if (requiresScalingCorrection) {
      h_blank_memory_start_position &= 0xfffe;
    }

    if (syncIsNegative) {
      uint16_t temp = h_sync_stop_position;
      h_sync_stop_position = h_sync_start_position;
      h_sync_start_position = temp;
    }

    GBS::VDS_HSYNC_RST::write(bestHTotal);
    GBS::VDS_HS_ST::write(h_sync_start_position);
    GBS::VDS_HS_SP::write(h_sync_stop_position);
    GBS::VDS_DIS_HB_ST::write(h_blank_display_start_position);
    GBS::VDS_DIS_HB_SP::write(h_blank_display_stop_position);
    GBS::VDS_HB_ST::write(h_blank_memory_start_position);
    GBS::VDS_HB_SP::write(h_blank_memory_stop_position);
    // IF htotal to pll divider (number of sampled pixels)
    //GBS::IF_HSYNC_RST::write((GBS::PLLAD_MD::read() / 2) - 2);
    //GBS::IF_LINE_SP::write((GBS::PLLAD_MD::read() / 2) - 1);
    // IF 1_1a to minimum
    //GBS::IF_HB_SP2::write(0x08);
  }
  SerialM.print("Base: "); SerialM.print(orig_htotal);
  SerialM.print(" Best: "); SerialM.println(bestHTotal);
  // todo: websocket breaks on this if diffHTotal is negative
  //SerialM.print(" Diff: "); SerialM.println(diffHTotal);
}

void doPostPresetLoadSteps() {
  // to decide: prevent patching already customized presets again ?
  if (rto->videoStandardInput > 0) {
    GBS::SP_H_PROTECT::write(0);
    GBS::SP_DIS_SUB_COAST::write(0);
  }
  if (rto->videoStandardInput == 3) { // ED YUV 60
      // p-scan ntsc, need to double adc data rate and halve vds scaling
    GBS::PLLAD_KS::write(1); // 5_16
    GBS::ADC_CLK_ICLK2X::write(0);
    uint16_t newVerticalScale = GBS::VDS_VSCALE::read() << 1;
    if (newVerticalScale > 1023) { newVerticalScale = 1023; }
    GBS::VDS_VSCALE::write(newVerticalScale);
  }
  else if (rto->videoStandardInput == 4) { // ED YUV 50
      // p-scan ntsc, need to double adc data rate and halve vds scaling
    GBS::PLLAD_KS::write(1); // 5_16
    GBS::ADC_CLK_ICLK2X::write(0);
    uint16_t newVerticalScale = GBS::VDS_VSCALE::read() << 1;
    if (newVerticalScale > 1023) { newVerticalScale = 1023; }
    GBS::VDS_VSCALE::write(newVerticalScale);
  }
  else if (rto->videoStandardInput == 5) { // 720p
    GBS::SP_HD_MODE::write(1); // tri level sync
    GBS::ADC_CLK_ICLK2X::write(0);
    GBS::PLLAD_KS::write(0); // 5_16
    GBS::VDS_VSCALE::write(768); // hardcoded for now
    GBS::VDS_HSCALE::write(683); // hardcoded for now
    GBS::IF_PRGRSV_CNTRL::write(1);
    GBS::IF_HS_DEC_FACTOR::write(0);
    // next: disable line doubling etc, IF stuff
    //GBS::IF_VB_ST::write(0);
    //GBS::IF_VB_SP::write(0x10); // v. position

    //GBS::IF_HB_SP2::write(0xf8);
    //GBS::PLLAD_FS::write(1); // high gain
    //GBS::PLLAD_ICP::write(7); // maximum charge pump current
    //GBS::VDS_VSCALE::write(804);
    //GBS::SP_CS_HS_ST::write(0x80);
    //GBS::SP_CS_HS_SP::write(0xa0);
  }
  else if (rto->videoStandardInput == 6) { // 1080i
    GBS::SP_HD_MODE::write(1); // tri level sync
    GBS::ADC_CLK_ICLK2X::write(0);
    GBS::PLLAD_KS::write(0); // 5_16
    //SP_CS_CLP_ST 0x58  ?
    //SP_CS_CLP_SP 0x62 ?
    GBS::IF_PRGRSV_CNTRL::write(1);
    GBS::IF_HS_DEC_FACTOR::write(0);
    //GBS::IF_VB_ST::write(0);
    //GBS::IF_VB_SP::write(0x10); // v. position

    //GBS::IF_HB_SP2::write(216); // todo: (s1_1a) position depends on preset
    //GBS::PLLAD_ICP::write(6); // high charge pump current
    //GBS::PLLAD_FS::write(1); // high gain
    //GBS::VDS_VSCALE::write(583);
    //GBS::SP_CS_HS_ST::write(0x50);
    //GBS::SP_CS_HS_SP::write(0x60);
  }

  if (rto->videoStandardInput != 15) setSpParameters();
  setAndUpdateSogLevel(rto->currentLevelSOG);

  // 0 segment
  GBS::DAC_RGBS_PWDNZ::write(0); // disable DAC here, enable later (should already be off though)
  GBS::IF_INI_ST::write(GBS::IF_HSYNC_RST::read() - 2); // IF initial position seems to be "ht" (on S1_0d)
  // ADC
  GBS::ADC_TR_RSEL::write(0); // in case it was set
  GBS::ADC_TR_ISEL::write(0); // in case it was set
  // high color gain so auto adjust can work on it
  if (uopt->enableAutoGain == 1) {
    GBS::ADC_RGCTRL::write(0x3c);
    GBS::ADC_GGCTRL::write(0x3c);
    GBS::ADC_BGCTRL::write(0x3c);
  }
  if (rto->inputIsYpBpR == true) {
    applyYuvPatches();
  }
  else {
    applyRGBPatches();
  }
  GBS::PLLAD_R::write(2);
  GBS::PLLAD_S::write(2);
  GBS::PLLAD_PDZ::write(1); // in case it was off
  //update rto phase variables
  rto->phaseADC = GBS::PA_ADC_S::read();
  rto->phaseSP = GBS::PA_SP_S::read();
  GBS::DEC_WEN_MODE::write(1); // keeps ADC phase much more consistent. around 4 lock positions vs totally random
  GBS::DEC_IDREG_EN::write(1);
  // jitter sync off for all modes
  GBS::SP_JITTER_SYNC::write(0);

  resetPLLAD(); // turns on pllad
  delay(20);
  resetDigital();

  rto->syncLockEnabled = true; // will re-detect whether debug wire is present
  Menu::init();
  enableDebugPort();

  GBS::PAD_SYNC_OUT_ENZ::write(1); // delay sync output
  enableVDS();
  FrameSync::reset();
  rto->syncLockFailIgnore = 2;
  //ResetSDRAM(); // already done in resetPLL
  GBS::DAC_RGBS_PWDNZ::write(1); // enable DAC
  unsigned long timeout = millis();
  while (getVideoMode() == 0 && millis() - timeout < 800) { delay(1); } // wifi stack // stability
  //SerialM.print("to1 is: "); SerialM.println(millis() - timeout);

  setPhaseSP(); setPhaseADC();
  updateCoastPosition();
  for (uint8_t i = 0; i < 8; i++) { // somehow this increases phase position reliability
    advancePhase();
  }
  GBS::PAD_SYNC_OUT_ENZ::write(0); // output sync > display goes on
  delay(2);
  GBS::INTERRUPT_CONTROL_00::write(0xff); // reset irq status
  GBS::INTERRUPT_CONTROL_00::write(0x00);
  SerialM.println("post preset done");
  rto->clampWasTurnedOff = 1; // make sure syncwatcher enables clamp
  rto->applyPresetDone = 1;
  rto->applyPresetDoneTime = millis();
}

void applyPresets(uint8_t result) {
  if (result == 1) {
    SerialM.println("60Hz ");
    if (uopt->presetPreference == 0) {
      writeProgramArrayNew(ntsc_240p);
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(ntsc_feedbackclock);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(ntsc_1280x720);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      SerialM.println("(custom)");
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(ntsc_1280x1024);
    }
#endif
  }
  else if (result == 2) {
    SerialM.println("50Hz ");
    if (uopt->presetPreference == 0) {
      writeProgramArrayNew(pal_240p);
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(pal_feedbackclock);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(pal_1280x720);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      SerialM.println("(custom)");
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(pal_240p);
    }
#endif
  }
  else if (result == 3) {
    SerialM.println("60Hz EDTV ");
    // ntsc base
    if (uopt->presetPreference == 0) {
      writeProgramArrayNew(ntsc_240p);
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(ntsc_feedbackclock);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(ntsc_1280x720);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      SerialM.println("(custom)");
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(ntsc_1280x1024);
    }
#endif
  }
  else if (result == 4) {
    SerialM.println("50Hz EDTV ");
    // pal base
    if (uopt->presetPreference == 0) {
      writeProgramArrayNew(pal_240p);
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(pal_feedbackclock);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(pal_1280x720);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      SerialM.println("(custom)");
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(pal_240p);
    }
#endif
  }
  else if (result == 5) {
    SerialM.println("720p 60Hz HDTV ");
    if (uopt->presetPreference == 0) {
      writeProgramArrayNew(ntsc_240p);
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(ntsc_feedbackclock);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(ntsc_1280x720);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      SerialM.println("(custom)");
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(ntsc_1280x1024);
    }
#endif
  }
  else if (result == 6) {
    SerialM.println("1080i 60Hz HDTV ");
    if (uopt->presetPreference == 0) {
      writeProgramArrayNew(ntsc_240p);
    }
    else if (uopt->presetPreference == 1) {
      writeProgramArrayNew(ntsc_feedbackclock);
    }
    else if (uopt->presetPreference == 3) {
      writeProgramArrayNew(ntsc_1280x720);
    }
#if defined(ESP8266)
    else if (uopt->presetPreference == 2) {
      SerialM.println("(custom)");
      const uint8_t* preset = loadPresetFromSPIFFS(result);
      writeProgramArrayNew(preset);
    }
    else if (uopt->presetPreference == 4) {
      writeProgramArrayNew(ntsc_1280x1024);
    }
#endif
  }
  else if (result == 15) {
    SerialM.println("RGBHV bypass ");
    writeProgramArrayNew(ntsc_240p);
    bypassModeSwitch_RGBHV();
  }
  else {
    SerialM.println("Unknown timing! ");
    rto->videoStandardInput = 0; // mark as "no sync" for syncwatcher
    inputAndSyncDetect();
    //resetModeDetect();
    delay(300);
    return;
  }

  rto->videoStandardInput = result;
  doPostPresetLoadSteps();
}

void enableDeinterlacer() {
  if (rto->currentSyncProcessorMode != 3) GBS::SFTRST_DEINT_RSTZ::write(1);
  rto->deinterlacerWasTurnedOff = false;
}

void disableDeinterlacer() {
  GBS::SFTRST_DEINT_RSTZ::write(0);
  rto->deinterlacerWasTurnedOff = true;
}

void disableVDS() {
  GBS::SFTRST_VDS_RSTZ::write(0);
}

void enableVDS() {
  if (rto->currentSyncProcessorMode != 3) {
    GBS::SFTRST_VDS_RSTZ::write(0); delay(3);
    GBS::SFTRST_VDS_RSTZ::write(1); delay(3);
  }
}

static uint8_t getVideoMode() {
  uint8_t detectedMode = 0;

  if (rto->videoStandardInput == 15) { // check RGBHV first
    detectedMode = GBS::STATUS_16::read();
    if ((detectedMode & 0x0a) > 0) { // bit 1 or 3 active?
      return 15; // still RGBHV bypass
    }
  }

  detectedMode = GBS::STATUS_00::read();
  // note: if stat0 == 0x07, it's supposedly stable. if we then can't find a mode, it must be an MD problem
  detectedMode &= 0x7f; // was 0x78 but 720p reports as 0x07
  if ((detectedMode & 0x08) == 0x08) return 1; // ntsc interlace (progressive also, easier to deal with)
  if ((detectedMode & 0x20) == 0x20) return 2; // pal interlace
  if ((detectedMode & 0x10) == 0x10) return 3; // edtv 60 progressive
  if ((detectedMode & 0x40) == 0x40) return 4; // edtv 50 progressive

  detectedMode = GBS::STATUS_03::read();
  if ((detectedMode & 0x10) == 0x10) { return 5; } // hdtv 720p
  detectedMode = GBS::STATUS_04::read();
  if ((detectedMode & 0x20) == 0x20) { // hd mode on
    if ((detectedMode & 0x10) == 0x10 || (detectedMode & 0x01) == 0x01) {
      return 6; // hdtv 1080p or i
    }
  }

  return 0; // unknown mode
}

// if testbus has 0x05, sync is present and line counting active. if it has 0x04, sync is present but no line counting
boolean getSyncPresent() {
  uint8_t debug_backup = GBS::TEST_BUS_SEL::read();
  GBS::TEST_BUS_SEL::write(0xa);
  uint16_t readout = GBS::TEST_BUS::read();
  if (((readout & 0x0500) == 0x0500) || ((readout & 0x0500) == 0x0400)) {
    GBS::TEST_BUS_SEL::write(debug_backup);
    return true;
  }
  GBS::TEST_BUS_SEL::write(debug_backup);
  return false;
}

boolean getSyncStable() {
  if (rto->videoStandardInput == 15) { // check RGBHV first
    if (GBS::STATUS_MISC_PLLAD_LOCK::read() == 1) {
      return true;
    }
    else {
      return false;
    }
  }

  uint8_t debug_backup = GBS::TEST_BUS_SEL::read();
  GBS::TEST_BUS_SEL::write(0xa);
  //todo: fix this to include other valids, ie RGBHV has 4 or 6
  if ((GBS::TEST_BUS::read() & 0x0500) == 0x0500) {
    GBS::TEST_BUS_SEL::write(debug_backup);
    return true;
  }
  GBS::TEST_BUS_SEL::write(debug_backup);
  return false;
}

void togglePhaseAdjustUnits() {
  GBS::PA_SP_BYPSZ::write(0); // yes, 0 means bypass on here
  GBS::PA_SP_BYPSZ::write(1);
}

void advancePhase() {
  rto->phaseADC = (rto->phaseADC + 2) & 0x1f;
  setPhaseADC();
}

void setPhaseSP() {
  GBS::PA_SP_LAT::write(0); // latch off
  GBS::PA_SP_S::write(rto->phaseSP);
  GBS::PA_SP_LAT::write(1); // latch on
}

void setPhaseADC() {
  GBS::PA_ADC_LAT::write(0);
  GBS::PA_ADC_S::write(rto->phaseADC);
  GBS::PA_ADC_LAT::write(1);
}

void updateCoastPosition() {
  if (rto->videoStandardInput <= 2) { // including 0 (reset condition)
    GBS::SP_PRE_COAST::write(0x0a);
    GBS::SP_POST_COAST::write(0x11);
  }
  else if (rto->videoStandardInput < 15) {
    GBS::SP_PRE_COAST::write(0x05);
    GBS::SP_POST_COAST::write(0x05);
  }
  else {
    GBS::SP_PRE_COAST::write(0x00);
    GBS::SP_POST_COAST::write(0x00);
  }

  //int16_t inHlength = 0;
  //uint8_t i = 0;
  //if (rto->videoStandardInput == 15) return;
  //for (; i < 8; i++) {
  //    inHlength += ((GBS::HPERIOD_IF::read() + 1) & 0xfffe); // psx jitters between 427, 428
  //}
  //inHlength = inHlength >> 1; // /8 , *4

  //if (inHlength > 0) {
  //    GBS::SP_H_CST_SP::write(inHlength >> 2); //snes minimum: inHlength -12 (only required in 239 mode)
  //    GBS::SP_H_CST_ST::write(inHlength >> 2);
  //}
}

void updateClampPosition() {
  // update: these are all dependent on the h: value
  if (rto->videoStandardInput == 0) {
    GBS::SP_CLAMP_INV_REG::write(0); // clamp normal
    GBS::SP_CS_CLP_ST::write(0x10);
    GBS::SP_CS_CLP_SP::write(0x27);
  }
  else if (rto->inputIsYpBpR && rto->videoStandardInput != 15) {
    // YUV
    GBS::SP_CLAMP_INV_REG::write(0); // clamp normal
    GBS::SP_CS_CLP_ST::write(0x04); // default positions
    GBS::SP_CS_CLP_SP::write(0x10);
    if (rto->videoStandardInput == 5 || rto->videoStandardInput == 6) { // 720p, 1080i
      GBS::SP_CS_CLP_ST::write(0x58);
      GBS::SP_CS_CLP_SP::write(0x62);
    }
  }
  else if (!rto->inputIsYpBpR && rto->videoStandardInput != 15) {
    //RGBS
    // standard definition RGBS: clamp on sync tip
    GBS::SP_CLAMP_INV_REG::write(1); // invert clamp: positioning on sync tip more accurate
    uint16_t inHlength = GBS::STATUS_SYNC_PROC_HTOTAL::read();
    if (inHlength < 100 || inHlength > 2800) { SerialM.println("!!B"); } // assert: must be valid
    GBS::SP_CS_CLP_ST::write(0);
    GBS::SP_CS_CLP_SP::write(inHlength - (inHlength >> 5));
  }
  else if (rto->videoStandardInput == 15) {
    //RGBHV bypass
    GBS::SP_CLAMP_INV_REG::write(0); // clamp normal
    GBS::SP_CS_CLP_ST::write(8);
    GBS::SP_CS_CLP_SP::write(18);
  }
}

// use t5t00t2 and adjust t5t11t5 to find this sources ideal sampling clock for this preset (affected by htotal)
// 2431 for psx, 2437 for MD
// in this mode, sampling clock is free to choose
void passThroughWithIFModeSwitch() {
  SerialM.println("pass-through");
  // first load default presets
  if (rto->videoStandardInput == 2 || rto->videoStandardInput == 4) {
    writeProgramArrayNew(pal_240p);
    doPostPresetLoadSteps();
  }
  else {
    writeProgramArrayNew(ntsc_240p);
    doPostPresetLoadSteps();
  }
  if (rto->outModeSynclock == 0) { // then enable pass-through mode
    delay(100); // stability
    uint16_t lineLength = GBS::STATUS_SYNC_PROC_HTOTAL::read();
    //GBS::PLL_MS::write(0x00); // memory clock 108mhz (many boards don't like fb clock) // not sure if required
    GBS::OUT_SYNC_SEL::write(2);
    GBS::VDS_HB_ST::write(0x30);
    GBS::VDS_HB_SP::write((lineLength / 32) & 0xffe);
    GBS::VDS_HS_ST::write(0);
    GBS::VDS_HS_SP::write(lineLength / 8);
    setDisplayHblankStartPosition(lineLength - (lineLength / 32));
    setDisplayHblankStopPosition(lineLength / 8);
    GBS::VDS_HSYNC_RST::write(0xfff); // max
    GBS::VDS_VSYNC_RST::write(0x7ff); // max
    GBS::PLLAD_MD::write(0x911); // psx 320 pix
    latchPLLAD();
    delay(10);
    GBS::PB_BYPASS::write(1);
    GBS::VDS_HSCALE_BYPS::write(1);
    GBS::VDS_VSCALE_BYPS::write(1);
    GBS::SP_HS_LOOP_SEL::write(0); // (5_57_6), is 1 normally
    GBS::PLL648_CONTROL_01::write(0x35);
    rto->phaseSP = 6; setPhaseSP();
    GBS::SP_SDCS_VSST_REG_H::write(0x00); // S5_3B
    GBS::SP_SDCS_VSSP_REG_H::write(0x00); // S5_3B
    GBS::SP_SDCS_VSST_REG_L::write(3); // S5_3F
    GBS::SP_SDCS_VSSP_REG_L::write(12); // S5_40 (test with interlaced sources)
    // IF
    GBS::IF_HS_TAP11_BYPS::write(1); // 1_02 bit 4 filter off looks better
    GBS::IF_HS_Y_PDELAY::write(3); // 1_02 bits 5+6
    GBS::IF_LD_RAM_BYPS::write(1);
    GBS::IF_HS_DEC_FACTOR::write(0);
    GBS::IF_HSYNC_RST::write(0x7ff); // (lineLength) // must be set for 240p at least
    GBS::IF_HBIN_SP::write(0x02); // must be even for 240p, adjusts left border at 0xf1+
    GBS::IF_HB_ST::write(0); // S1_10
    delay(30);
    SyncProcessorOffOn();
    GBS::VDS_SYNC_EN::write(1); // VDS sync to synclock
    GBS::SFTRST_VDS_RSTZ::write(0);
    GBS::SFTRST_VDS_RSTZ::write(1);
    delay(30);
    rto->outModeSynclock = 1;
  }
  else { // switch back
    writeProgramArrayNew(ntsc_240p);
    doPostPresetLoadSteps();
    rto->outModeSynclock = 0;
  }
}

void bypassModeSwitch_SOG() {
  static uint16_t oldPLLAD = 0;
  static uint8_t oldPLL648_CONTROL_01 = 0;
  static uint8_t oldPLLAD_ICP = 0;
  static uint8_t old_0_46 = 0;
  static uint8_t old_5_3f = 0;
  static uint8_t old_5_40 = 0;
  static uint8_t old_5_3e = 0;

  if (GBS::DAC_RGBS_ADC2DAC::read() == 0) {
    oldPLLAD = GBS::PLLAD_MD::read();
    oldPLL648_CONTROL_01 = GBS::PLL648_CONTROL_01::read();
    oldPLLAD_ICP = GBS::PLLAD_ICP::read();
    old_0_46 = GBS::RESET_CONTROL_0x46::read();
    old_5_3f = GBS::SP_SDCS_VSST_REG_L::read();
    old_5_40 = GBS::SP_SDCS_VSSP_REG_L::read();
    old_5_3e = GBS::SP_CS_0x3E::read();

    // WIP
    //GBS::PLLAD_ICP::write(5);
    GBS::OUT_SYNC_SEL::write(2); // S0_4F, 6+7
    GBS::DAC_RGBS_ADC2DAC::write(1); // S0_4B, 2
    GBS::SP_HS_LOOP_SEL::write(0); // retiming enable
    //GBS::SP_CS_0x3E::write(0x20); // sub coast off, ovf protect off
    //GBS::PLL648_CONTROL_01::write(0x35); // display clock
    //GBS::PLLAD_MD::write(1802);
    GBS::SP_SDCS_VSST_REG_L::write(0x00);
    GBS::SP_SDCS_VSSP_REG_L::write(0x09);
    //GBS::RESET_CONTROL_0x46::write(0); // none required
  }
  else {
    GBS::OUT_SYNC_SEL::write(0);
    GBS::DAC_RGBS_ADC2DAC::write(0);
    GBS::SP_HS_LOOP_SEL::write(1);

    if (oldPLLAD_ICP != 0) GBS::PLLAD_ICP::write(oldPLLAD_ICP);
    if (oldPLL648_CONTROL_01 != 0) GBS::PLL648_CONTROL_01::write(oldPLL648_CONTROL_01);
    if (oldPLLAD != 0) GBS::PLLAD_MD::write(oldPLLAD);
    if (old_5_3e != 0) GBS::SP_CS_0x3E::write(old_5_3e);
    if (old_5_3f != 0) GBS::SP_SDCS_VSST_REG_L::write(old_5_3f);
    if (old_5_40 != 0) GBS::SP_SDCS_VSSP_REG_L::write(old_5_40);
    if (old_0_46 != 0) GBS::RESET_CONTROL_0x46::write(old_0_46);
  }
}

void bypassModeSwitch_RGBHV() {
  uint8_t readout;

  rto->videoStandardInput = 15; // making sure

  writeOneByte(0xF0, 0);
  writeOneByte(0x40, 0x0c); // drop memory clock
  writeOneByte(0x41, 0x35); // PLL clock = from ADC
  readFromRegister(0x4b, 1, &readout); writeOneByte(0x4b, readout | (1 << 2)); // When = 1, enable ADC (with decimation) to DAC directly t0t4bt2
  // correction: this could be t4t2bt3 on (playback bypass > direct)

  readFromRegister(0x4f, 1, &readout); writeOneByte(0x4f, readout | (1 << 7)); // When = 10, H/V sync output are from sync processor

  writeOneByte(0xF0, 3);
  readFromRegister(0x1a, 1, &readout); writeOneByte(0x1a, readout | (1 << 4)); // frame lock on (use source timing directly)

  // oversampling 1x (off)
  GBS::ADC_CLK_ICLK2X::write(0);
  GBS::ADC_CLK_ICLK1X::write(0);
  GBS::PLLAD_KS::write(1); // 0 - 3
  GBS::PLLAD_CKOS::write(0); // 0 - 3
  GBS::DEC1_BYPS::write(1); // 1 = bypassed
  GBS::DEC2_BYPS::write(1);
  GBS::ADC_FLTR::write(0);

  GBS::ADC_SOGEN::write(0); // 5_02 bit 0
  GBS::SP_SOG_MODE::write(0); // 5_56 bit 0
  GBS::SP_EXT_SYNC_SEL::write(0); // connect HV input ( 5_20 bit 3 )
  GBS::SP_CLAMP_MANUAL::write(1); // 1: clamp turn on off by control by software (default)
  GBS::SP_HS_LOOP_SEL::write(0); // off for now but do tests later
  //GBS::SP_CS_P_SWAP::write(1); // 5_3e bit 0
  GBS::SP_DIS_SUB_COAST::write(1);

  GBS::PLLAD_ICP::write(6);
  GBS::PLLAD_FS::write(0); // low gain
  GBS::PLLAD_MD::write(1856); // 1349 perfect for for 1280x+ ; 1856 allows lower res to detect

  syncProcessorModeVGA();
  resetDigital(); // this will leave 0_46 reset controls with 0 (bypassed blocks disabled)
}

void applyYuvPatches() {
  GBS::ADC_RYSEL_R::write(1); // midlevel clamp red
  GBS::ADC_RYSEL_B::write(1); // midlevel clamp blue
  //GBS::ADC_AUTO_OFST_EN::write(1);
  //GBS::IF_AUTO_OFST_EN::write(1);
  GBS::IF_MATRIX_BYPS::write(1);
  // colors
  GBS::VDS_U_OFST::write(3);
  GBS::VDS_V_OFST::write(3);
}

// undo yuvpatches if necessary
void applyRGBPatches() {
  GBS::ADC_AUTO_OFST_EN::write(0);
  GBS::ADC_RYSEL_R::write(0); // gnd clamp red
  GBS::ADC_RYSEL_B::write(0); // gnd clamp blue
  //GBS::ADC_AUTO_OFST_EN::write(0);
  //GBS::IF_AUTO_OFST_EN::write(0);
  GBS::IF_MATRIX_BYPS::write(0);
  // colors
  GBS::VDS_U_OFST::write(0);
  GBS::VDS_V_OFST::write(0);
}

void doAutoGain() {
  uint8_t r_found = 0, g_found = 0, b_found = 0;
  uint16_t value = 0;

  GBS::DEC_TEST_SEL::write(1); // 0x9c
  for (uint8_t i = 0; i < 4; i++) {
    value = GBS::TEST_BUS::read();
    if ((value & 0x00ff) == 0x00ff || (value & 0x00ff) == 0x007f) {
      b_found++;
    }
    else if ((value & 0xff00) == 0xff00 || (value & 0xff00) == 0x007f) {
      g_found++;
    }
  }

  GBS::DEC_TEST_SEL::write(3); // 0xbc
  for (uint8_t i = 0; i < 4; i++) {
    value = GBS::TEST_BUS::read() & 0x00ff;
    if (value == 0x00ff || value == 0x007f) {
      r_found++;
    }
  }

  //if (r_found > 2 || g_found > 2 || b_found > 2) {
  //    if (GBS::ADC_RGCTRL::read() < 0xA0) {
  //        GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() + 2); // larger steps?
  //        GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() + 2);
  //        GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() + 2);
  //        //SerialM.print("ADC gain: "); SerialM.println(GBS::ADC_RGCTRL::read(), HEX);
  //    }
  //}
  if (r_found > 2) {
    if (GBS::ADC_RGCTRL::read() < 0x90) {
      GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() + 2);
      //SerialM.print("r");
      //SerialM.print("RGCTRL: "); SerialM.println(GBS::ADC_RGCTRL::read(), HEX);
    }
  }
  if (g_found > 2) {
    if (GBS::ADC_GGCTRL::read() < 0x90) {
      GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() + 2);
      //SerialM.print("g");
      //SerialM.print("GGCTRL: "); SerialM.println(GBS::ADC_GGCTRL::read(), HEX);
    }
  }
  if (b_found > 2) {
    if (GBS::ADC_BGCTRL::read() < 0x90) {
      GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() + 2);
      //SerialM.print("b");
      //SerialM.print("BGCTRL: "); SerialM.println(GBS::ADC_BGCTRL::read(), HEX);
    }
  }
}

void startWire() {
  Wire.begin();
  // The i2c wire library sets pullup resistors on by default. Disable this so that 5V MCUs aren't trying to drive the 3.3V bus.
#if defined(ESP8266)
  pinMode(SCL, OUTPUT_OPEN_DRAIN);
  pinMode(SDA, OUTPUT_OPEN_DRAIN);
  Wire.setClock(100000); // TV5725 supports 400kHz // but 100kHz is better suited
#else
  digitalWrite(SCL, LOW);
  digitalWrite(SDA, LOW);
  Wire.setClock(100000);
#endif
}

void setup() {
  Serial.begin(115200); // set Arduino IDE Serial Monitor to the same 115200 bauds!
  rto->webServerEnabled = true; // control gbs-control(:p) via web browser, only available on wifi boards.
  rto->webServerStarted = false; // make sure this is set
#if defined(ESP8266)
  // SDK enables WiFi and uses stored credentials to auto connect. This can't be turned off.
  // Correct the hostname while it is still in CONNECTING state
  //wifi_station_set_hostname("gbscontrol"); // SDK version
  WiFi.hostname("gbscontrol");

  // start web services as early in boot as possible > greater chance to get a websocket connection in time for logging startup
  if (rto->webServerEnabled) {
    start_webserver();
    WiFi.setOutputPower(14.0f); // float: min 0.0f, max 20.5f // reduced from max, but still strong
    rto->webServerStarted = true;
    unsigned long initLoopStart = millis();
    while (millis() - initLoopStart < 2000) {
      persWM.handleWiFi();
      dnsServer.processNextRequest();
      server.handleClient();
      webSocket.loop();
      delay(1); // allow some time for the ws server to find clients currently trying to reconnect
    }
  }
  else {
    //WiFi.disconnect(); // deletes credentials
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    delay(1);
  }
#endif

  Serial.setTimeout(10);
  Serial.println("starting");
  // user options // todo: could be stored in Arduino EEPROM. Other MCUs have SPIFFS
  uopt->presetPreference = 0; // normal, 720p, fb, custom, 1280x1024
  uopt->presetGroup = 0; //
  uopt->enableFrameTimeLock = 0; // permanently adjust frame timing to avoid glitch vertical bar. does not work on all displays!
  uopt->frameTimeLockMethod = 0; // compatibility with more displays
  uopt->enableAutoGain = 0; // todo: web ui option
  // run time options
  rto->allowUpdatesOTA = false; // ESP over the air updates. default to off, enable via web interface
  rto->webSocketConnected = false;
  rto->syncLockEnabled = true;  // automatically find the best horizontal total pixel value for a given input timing
  rto->syncLockFailIgnore = 2; // allow syncLock to fail x-1 times in a row before giving up (sync glitch immunity)
  rto->forceRetime = false;
  rto->syncWatcherEnabled = true;  // continously checks the current sync status. required for normal operation
  rto->phaseADC = 16;
  rto->phaseSP = 6;

  // the following is just run time variables. don't change!
  rto->inputIsYpBpR = false;
  rto->videoStandardInput = 0;
  rto->currentSyncProcessorMode = 0;
  rto->outModeSynclock = false;
  rto->deinterlacerWasTurnedOff = false;
  rto->clampWasTurnedOff = false;
  if (!rto->webServerEnabled) rto->webServerStarted = false;
  rto->printInfos = false;
  rto->sourceDisconnected = true;
  rto->applyPresetDone = false;
  rto->applyPresetDoneTime = millis();
  rto->currentLevelSOG = 8;
  rto->sourceVLines = 0;

  globalCommand = 0; // web server uses this to issue commands

  pinMode(DEBUG_IN_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  LEDON; // enable the LED, lets users know the board is starting up
  delay(500); // give the entire system some time to start up.

#if defined(ESP8266)
  //Serial.setDebugOutput(true); // if you want simple wifi debug info
  // file system (web page, custom presets, ect)
  if (!SPIFFS.begin()) {
    SerialM.println("SPIFFS Mount Failed");
  }
  else {
    // load userprefs.txt
    File f = SPIFFS.open("/userprefs.txt", "r");
    if (!f) {
      SerialM.println("userprefs open failed");
      uopt->presetPreference = 0;
      uopt->enableFrameTimeLock = 0;
      uopt->presetGroup = 0;
      uopt->frameTimeLockMethod = 0;
      saveUserPrefs(); // if this fails, there must be a spiffs problem
    }
    else {
      SerialM.println("userprefs open ok");
      //on a fresh / spiffs not formatted yet MCU:
      //userprefs.txt open ok //result[0] = 207 //result[1] = 207
      char result[4];
      result[0] = f.read(); result[0] -= '0'; // file streams with their chars..
      uopt->presetPreference = (uint8_t)result[0];
      SerialM.print("presetPreference = "); SerialM.println(uopt->presetPreference);
      if (uopt->presetPreference > 4) uopt->presetPreference = 0; // fresh spiffs ?

      result[1] = f.read(); result[1] -= '0';
      uopt->enableFrameTimeLock = (uint8_t)result[1]; // Frame Time Lock
      SerialM.print("FrameTime Lock = "); SerialM.println(uopt->enableFrameTimeLock);
      if (uopt->enableFrameTimeLock > 1) uopt->enableFrameTimeLock = 0; // fresh spiffs ?

      result[2] = f.read(); result[2] -= '0';
      uopt->presetGroup = (uint8_t)result[2];
      SerialM.print("presetGroup = "); SerialM.println(uopt->presetGroup); // custom preset group
      if (uopt->presetGroup > 4) uopt->presetGroup = 0;

      result[3] = f.read(); result[3] -= '0';
      uopt->frameTimeLockMethod = (uint8_t)result[3];
      SerialM.print("frameTimeLockMethod = "); SerialM.println(uopt->frameTimeLockMethod);
      if (uopt->frameTimeLockMethod > 1) uopt->frameTimeLockMethod = 0;

      f.close();
    }
  }
#else
  delay(500); // give the entire system some time to start up.
#endif
  startWire();

  // i2c can get stuck
  if (digitalRead(SDA) == 0) {
    unsigned long timeout = millis();
    while (digitalRead(SDA) == 0 && millis() - timeout < 5000) {
      static uint8_t result = 0;
      static boolean printDone = 0;
      if (!printDone) {
        SerialM.print("i2c: ");
        printDone = 1;
      }
      pinMode(SCL, INPUT); pinMode(SDA, INPUT);
      delay(100);
      pinMode(SCL, OUTPUT);
      for (int i = 0; i < 10; i++) {
        digitalWrite(SCL, HIGH); delayMicroseconds(5);
        digitalWrite(SCL, LOW); delayMicroseconds(5);
      }
      pinMode(SCL, INPUT);
      startWire();
      writeOneByte(0xf0, 0); readFromRegister(0x0c, 1, &result);
      Serial.print(result, HEX);
    }
    SerialM.print("\n");
  }

  zeroAll();
  GBS::TEST_BUS_EN::write(0); // to init some template variables
  GBS::TEST_BUS_SEL::write(0);
  
  setResetParameters();
  loadPresetMdSection(); // fills 1_60 to 1_83 (mode detect segment, mostly static)
  setAdcParameters();
  setSpParameters();
  setAndUpdateSogLevel(8);
  delay(300); // let everything settle first

  //rto->syncWatcherEnabled = false;
  //inputAndSyncDetect();

  // allows passive operation by disabling syncwatcher
  if (rto->syncWatcherEnabled == true) {
    for (uint8_t i = 0; i < 3; i++) {
      if (detectAndSwitchToActiveInput()) break;
      else if (i == 2) inputAndSyncDetect();
    }
  }
  LEDOFF; // new behaviour: only light LED on active sync
}

#ifdef HAVE_BUTTONS
#define INPUT_SHIFT 0
#define DOWN_SHIFT 1
#define UP_SHIFT 2
#define MENU_SHIFT 3

static const uint8_t historySize = 32;
static const uint16_t buttonPollInterval = 100; // microseconds
static uint8_t buttonHistory[historySize];
static uint8_t buttonIndex;
static uint8_t buttonState;
static uint8_t buttonChanged;

uint8_t readButtons(void) {
  return ~((digitalRead(INPUT_PIN) << INPUT_SHIFT) |
    (digitalRead(DOWN_PIN) << DOWN_SHIFT) |
    (digitalRead(UP_PIN) << UP_SHIFT) |
    (digitalRead(MENU_PIN) << MENU_SHIFT));
}

void debounceButtons(void) {
  buttonHistory[buttonIndex++ % historySize] = readButtons();
  buttonChanged = 0xFF;
  for (uint8_t i = 0; i < historySize; ++i)
    buttonChanged &= buttonState ^ buttonHistory[i];
  buttonState ^= buttonChanged;
}

bool buttonDown(uint8_t pos) {
  return (buttonState & (1 << pos)) && (buttonChanged & (1 << pos));
}

void handleButtons(void) {
  debounceButtons();
  if (buttonDown(INPUT_SHIFT))
    Menu::run(MenuInput::BACK);
  if (buttonDown(DOWN_SHIFT))
    Menu::run(MenuInput::DOWN);
  if (buttonDown(UP_SHIFT))
    Menu::run(MenuInput::UP);
  if (buttonDown(MENU_SHIFT))
    Menu::run(MenuInput::FORWARD);
}
#endif

void loop() {
  static uint8_t readout = 0;
  static uint8_t segment = 0;
  static uint8_t inputRegister = 0;
  static uint8_t inputToogleBit = 0;
  static uint8_t inputStage = 0;
  static uint16_t noSyncCounter = 0;
  static unsigned long lastTimeSyncWatcher = millis();
  static unsigned long lastVsyncLock = millis();
  static unsigned long lastTimeSourceCheck = millis();
#ifdef HAVE_BUTTONS
  static unsigned long lastButton = micros();
#endif

#if defined(ESP8266)
  if (rto->webServerEnabled && rto->webServerStarted) {
    persWM.handleWiFi(); // if connected, returns instantly. otherwise it reconnects or opens AP
    dnsServer.processNextRequest();
    server.handleClient();
    webSocket.loop();
    // if there's a control command from the server, globalCommand will now hold it.
    // process it in the parser, then reset to 0 at the end of the sketch.
  }

  if (rto->allowUpdatesOTA) {
    ArduinoOTA.handle();
  }
#endif

#ifdef HAVE_BUTTONS
  if (micros() - lastButton > buttonPollInterval) {
    lastButton = micros();
    handleButtons();
  }
#endif

  if (Serial.available() || globalCommand != 0) {
    switch (globalCommand == 0 ? Serial.read() : globalCommand) {
    case ' ':
      // skip spaces
      inputStage = 0; // reset this as well
      break;
    case 'd':
      for (int segment = 0; segment <= 5; segment++) {
        dumpRegisters(segment);
      }
      SerialM.println("};");
      break;
    case '+':
      SerialM.println("hor. +");
      shiftHorizontalRight();
      //shiftHorizontalRightIF(4);
      break;
    case '-':
      SerialM.println("hor. -");
      shiftHorizontalLeft();
      //shiftHorizontalLeftIF(4);
      break;
    case '*':
      shiftVerticalUpIF();
      break;
    case '/':
      shiftVerticalDownIF();
      break;
    case 'z':
      SerialM.println("scale+");
      scaleHorizontalLarger();
      break;
    case 'h':
      SerialM.println("scale-");
      scaleHorizontalSmaller();
      break;
    case 'q':
      resetDigital();
      enableVDS();
      break;
    case 'D':
      //debug stuff: shift h blanking into good view
      if (GBS::ADC_ROFCTRL_FAKE::read() == 0x00) { // "remembers" debug view 
        shiftHorizontal(700, false);
        GBS::VDS_PK_Y_H_BYPS::write(0); // enable peaking
        GBS::VDS_PK_LB_CORE::write(0); // peaking high pass open
        GBS::ADC_ROFCTRL_FAKE::write(GBS::ADC_ROFCTRL::read()); // backup
        GBS::ADC_GOFCTRL_FAKE::write(GBS::ADC_GOFCTRL::read());
        GBS::ADC_BOFCTRL_FAKE::write(GBS::ADC_BOFCTRL::read());
        GBS::ADC_ROFCTRL::write(0x08);
        GBS::ADC_GOFCTRL::write(0x08);
        GBS::ADC_BOFCTRL::write(0x08);
        // enhance!
      }
      else {
        shiftHorizontal(700, true);
        GBS::VDS_PK_Y_H_BYPS::write(1);
        GBS::VDS_PK_LB_CORE::write(1);
        GBS::ADC_ROFCTRL::write(GBS::ADC_ROFCTRL_FAKE::read()); // restore ..
        GBS::ADC_GOFCTRL::write(GBS::ADC_GOFCTRL_FAKE::read());
        GBS::ADC_BOFCTRL::write(GBS::ADC_BOFCTRL_FAKE::read());
        GBS::ADC_ROFCTRL_FAKE::write(0); // .. and clear
        GBS::ADC_GOFCTRL_FAKE::write(0);
        GBS::ADC_BOFCTRL_FAKE::write(0);
      }
      break;
    case 'C':
      SerialM.println("PLL: ICLK");
      //GBS::PB_MAST_FLAG_REG::write(0x37); // or set memory clock to 108mhz (0x00)
      GBS::PLL_MS::write(0);
      //GBS::MEM_CLK_DLYCELL_SEL::write(0);
      //GBS::MEM_ADR_DLY_REG::write(0x03); GBS::MEM_CLK_DLY_REG::write(0x03); // memory subtimings
      GBS::PLLAD_FS::write(1); // gain high
      GBS::PLLAD_ICP::write(3); // CPC was 5, but MD works with as low as 0 and it removes a glitch
      GBS::PLL_CKIS::write(1); // PLL use ICLK (instead of oscillator)
      latchPLLAD();
      GBS::VDS_HSCALE::write(512);
      delay(20);
      rto->syncLockFailIgnore = 2;
      FrameSync::reset(); // adjust htotal to new display clock
      rto->forceRetime = true;
      //applyBestHTotal(FrameSync::init());
      //GBS::VDS_FLOCK_EN::write(1); //risky
      //delay(30);
      break;
    case 'Y':
      writeProgramArrayNew(ntsc_1280x720);
      doPostPresetLoadSteps();
      break;
    case 'y':
      writeProgramArrayNew(pal_1280x720);
      doPostPresetLoadSteps();
      break;
    case 'P':
      //
      break;
    case 'p':
      //
      break;
    case 'k':
      bypassModeSwitch_RGBHV();
      //bypassModeSwitch_SOG();  // arduino space saving
      latchPLLAD();
      break;
    case 'K':
      passThroughWithIFModeSwitch();
      break;
    case 'T':
      SerialM.print("auto gain ");
      if (uopt->enableAutoGain == 0) {
        uopt->enableAutoGain = 1;
        GBS::ADC_RGCTRL::write(0x3c);
        GBS::ADC_GGCTRL::write(0x3c);
        GBS::ADC_BGCTRL::write(0x3c);
        SerialM.println("on");
      }
      else {
        uopt->enableAutoGain = 0;
        SerialM.println("off");
      }
      break;
    case 'e':
      writeProgramArrayNew(ntsc_240p);
      doPostPresetLoadSteps();
      break;
    case 'r':
      writeProgramArrayNew(pal_240p);
      doPostPresetLoadSteps();
      break;
    case '.':
      // timings recalculation with new bestHtotal
      FrameSync::reset();
      rto->syncLockFailIgnore = 2;
      rto->forceRetime = true;
      break;
    case 'j':
      resetPLL(); latchPLLAD(); //resetPLLAD();
      break;
    case 'v':
      rto->phaseSP += 2; rto->phaseSP &= 0x1f;
      SerialM.print("SP: "); SerialM.println(rto->phaseSP);
      setPhaseSP();
      setPhaseADC();
      break;
    case 'b':
      advancePhase(); latchPLLAD();
      SerialM.print("ADC: "); SerialM.println(rto->phaseADC);
      break;
    case 'n':
    {
      uint16_t pll_divider = GBS::PLLAD_MD::read();
      if (pll_divider < 4095) {
        pll_divider += 1;
        GBS::PLLAD_MD::write(pll_divider);

        //            uint8_t PLLAD_KS = GBS::PLLAD_KS::read();
        //            uint16_t line_length = GBS::PLLAD_MD::read();
        //            if (PLLAD_KS == 2) {
        //              line_length *= 1;
        //            }
        //            if (PLLAD_KS == 1) {
        //              line_length /= 2;
        //            }
        //
        //            line_length = line_length / ((rto->currentSyncProcessorMode > 0 ? 1 : 2)); // half of pll_divider, but in linedouble mode only

        SerialM.print("PLL div: "); SerialM.println(pll_divider, HEX);

        // regular output modes: apply IF corrections
        if (GBS::VDS_HSYNC_RST::read() != 0xfff) {
          uint16_t IF_HSYNC_RST = GBS::IF_HSYNC_RST::read(); // 1_0E
          GBS::IF_HSYNC_RST::write(IF_HSYNC_RST + 1);
          // IF HB new stuff
          GBS::IF_INI_ST::write(IF_HSYNC_RST - 1); // initial position seems to be "ht" (on S1_0d)
          //GBS::IF_LINE_ST::write(GBS::IF_LINE_ST::read() + 1);
          GBS::IF_LINE_SP::write(GBS::IF_LINE_SP::read() + 1); // 1_22
        }
        latchPLLAD();
      }
    }
    break;
    case 'a':
    {
      uint16_t VDS_HSYNC_RST = GBS::VDS_HSYNC_RST::read();
      GBS::VDS_HSYNC_RST::write(VDS_HSYNC_RST + 1);
      SerialM.print("HTotal++: "); SerialM.println(VDS_HSYNC_RST + 1);
    }
    break;
    case 'A':
      optimizeSogLevel();
      break;
    case 'M':
      zeroAll();
      break;
    case 'm':
      SerialM.print("syncwatcher ");
      if (rto->syncWatcherEnabled == true) {
        rto->syncWatcherEnabled = false;
        SerialM.println("off");
      }
      else {
        rto->syncWatcherEnabled = true;
        SerialM.println("on");
      }
      break;
    case ',':
#if defined(ESP8266) // Arduino space saving
      SerialM.println("----");
      getVideoTimings();
#endif
      break;
    case 'i':
      rto->printInfos = !rto->printInfos;
      break;
#if defined(ESP8266)
    case 'c':
      SerialM.println("OTA Updates on");
      initUpdateOTA();
      rto->allowUpdatesOTA = true;
      break;
#endif
    case 'u':
      ResetSDRAM();
      break;
    case 'f':
      SerialM.print("peaking ");
      if (GBS::VDS_PK_Y_H_BYPS::read() == 1) {
        GBS::VDS_PK_Y_H_BYPS::write(0);
        SerialM.println("on");
      }
      else {
        GBS::VDS_PK_Y_H_BYPS::write(1);
        SerialM.println("off");
      }
      break;
    case 'F':
      SerialM.print("ADC filter ");
      if (GBS::ADC_FLTR::read() > 0) {
        GBS::ADC_FLTR::write(0);
        SerialM.println("off");
      }
      else {
        GBS::ADC_FLTR::write(3);
        SerialM.println("on");
      }
      break;
    case 'L':
      //
      break;
    case 'l':
      SerialM.println("spOffOn");
      SyncProcessorOffOn();
      break;
    case 'W':
      uopt->enableFrameTimeLock = !uopt->enableFrameTimeLock;
      break;
    case 'E':
      //
      break;
    case '0':
      moveHS(1, true);
      break;
    case '1':
      moveHS(1, false);
      break;
    case '2':
      writeProgramArrayNew(pal_feedbackclock); // ModeLine "720x576@50" 27 720 732 795 864 576 581 586 625 -hsync -vsync
      doPostPresetLoadSteps();
      break;
    case '3':
      //
      break;
    case '4':
      scaleVertical(1, true);
      break;
    case '5':
      scaleVertical(1, false);
      break;
    case '6':
      GBS::IF_HBIN_SP::write(GBS::IF_HBIN_SP::read() - 4); // canvas move left
      break;
    case '7':
      GBS::IF_HBIN_SP::write(GBS::IF_HBIN_SP::read() + 4); // canvas move right
      break;
    case '8':
      //SerialM.println("invert sync");
      invertHS(); invertVS();
      break;
    case '9':
      writeProgramArrayNew(ntsc_feedbackclock);
      doPostPresetLoadSteps();
      break;
    case 'o':
    {
      switch (GBS::PLLAD_CKOS::read()) {
      case 0:
        SerialM.println("OSR 1x"); // oversampling ratio
        GBS::ADC_CLK_ICLK2X::write(0);
        GBS::ADC_CLK_ICLK1X::write(0);
        GBS::PLLAD_KS::write(2); // 0 - 3
        GBS::PLLAD_CKOS::write(2); // 0 - 3
        GBS::DEC1_BYPS::write(1); // 1 = bypassed
        GBS::DEC2_BYPS::write(1);
        break;
      case 1:
        SerialM.println("OSR 4x");
        GBS::ADC_CLK_ICLK2X::write(1);
        GBS::ADC_CLK_ICLK1X::write(1);
        GBS::PLLAD_KS::write(2); // 0 - 3
        GBS::PLLAD_CKOS::write(0); // 0 - 3
        GBS::DEC1_BYPS::write(0);
        GBS::DEC2_BYPS::write(0);
        break;
      case 2:
        SerialM.println("OSR 2x");
        GBS::ADC_CLK_ICLK2X::write(0);
        GBS::ADC_CLK_ICLK1X::write(1);
        GBS::PLLAD_KS::write(2); // 0 - 3
        GBS::PLLAD_CKOS::write(1); // 0 - 3
        GBS::DEC1_BYPS::write(1);
        GBS::DEC2_BYPS::write(0);
        break;
      default:
        break;
      }
      //resetPLLAD(); // just latching not good enough, shifts h offset
      //ResetSDRAM(); // sdram sometimes locks up going from x4 to x1
      // test!
      latchPLLAD();
    }
    break;
    case 'g':
      inputStage++;
      Serial.flush();
      // we have a multibyte command
      if (inputStage > 0) {
        if (inputStage == 1) {
          segment = Serial.parseInt();
          SerialM.print("G");
          SerialM.print(segment);
        }
        else if (inputStage == 2) {
          char szNumbers[3];
          szNumbers[0] = Serial.read(); szNumbers[1] = Serial.read(); szNumbers[2] = '\0';
          //char * pEnd;
          inputRegister = strtol(szNumbers, NULL, 16);
          SerialM.print("R0x");
          SerialM.print(inputRegister, HEX);
          if (segment <= 5) {
            writeOneByte(0xF0, segment);
            readFromRegister(inputRegister, 1, &readout);
            SerialM.print(" value: 0x"); SerialM.println(readout, HEX);
          }
          else {
            SerialM.println("abort");
          }
          inputStage = 0;
        }
      }
      break;
    case 's':
      inputStage++;
      Serial.flush();
      // we have a multibyte command
      if (inputStage > 0) {
        if (inputStage == 1) {
          segment = Serial.parseInt();
          SerialM.print("S");
          SerialM.print(segment);
        }
        else if (inputStage == 2) {
          char szNumbers[3];
          szNumbers[0] = Serial.read(); szNumbers[1] = Serial.read(); szNumbers[2] = '\0';
          //char * pEnd;
          inputRegister = strtol(szNumbers, NULL, 16);
          SerialM.print("R0x");
          SerialM.print(inputRegister, HEX);
        }
        else if (inputStage == 3) {
          char szNumbers[3];
          szNumbers[0] = Serial.read(); szNumbers[1] = Serial.read(); szNumbers[2] = '\0';
          //char * pEnd;
          inputToogleBit = strtol(szNumbers, NULL, 16);
          if (segment <= 5) {
            writeOneByte(0xF0, segment);
            readFromRegister(inputRegister, 1, &readout);
            SerialM.print(" (was 0x"); SerialM.print(readout, HEX); SerialM.print(")");
            writeOneByte(inputRegister, inputToogleBit);
            readFromRegister(inputRegister, 1, &readout);
            SerialM.print(" is now: 0x"); SerialM.println(readout, HEX);
          }
          else {
            SerialM.println("abort");
          }
          inputStage = 0;
        }
      }
      break;
    case 't':
      inputStage++;
      Serial.flush();
      // we have a multibyte command
      if (inputStage > 0) {
        if (inputStage == 1) {
          segment = Serial.parseInt();
          SerialM.print("T");
          SerialM.print(segment);
        }
        else if (inputStage == 2) {
          char szNumbers[3];
          szNumbers[0] = Serial.read(); szNumbers[1] = Serial.read(); szNumbers[2] = '\0';
          //char * pEnd;
          inputRegister = strtol(szNumbers, NULL, 16);
          SerialM.print("R0x");
          SerialM.print(inputRegister, HEX);
        }
        else if (inputStage == 3) {
          inputToogleBit = Serial.parseInt();
          SerialM.print(" Bit: "); SerialM.print(inputToogleBit);
          inputStage = 0;
          if ((segment <= 5) && (inputToogleBit <= 7)) {
            writeOneByte(0xF0, segment);
            readFromRegister(inputRegister, 1, &readout);
            SerialM.print(" (was 0x"); SerialM.print(readout, HEX); SerialM.print(")");
            writeOneByte(inputRegister, readout ^ (1 << inputToogleBit));
            readFromRegister(inputRegister, 1, &readout);
            SerialM.print(" is now: 0x"); SerialM.println(readout, HEX);
          }
          else {
            SerialM.println("abort");
          }
        }
      }
      break;
    case 'w':
    {
      Serial.flush();
      uint16_t value = 0;
      String what = Serial.readStringUntil(' ');

      if (what.length() > 5) {
        SerialM.println("abort");
        inputStage = 0;
        break;
      }
      value = Serial.parseInt();
      if (value < 4096) {
        SerialM.print("set "); SerialM.print(what); SerialM.print(" "); SerialM.println(value);
        if (what.equals("ht")) {
          set_htotal(value);
        }
        else if (what.equals("vt")) {
          set_vtotal(value);
        }
        else if (what.equals("hsst")) {
          setHSyncStartPosition(value);
        }
        else if (what.equals("hssp")) {
          setHSyncStopPosition(value);
        }
        else if (what.equals("hbst")) {
          setMemoryHblankStartPosition(value);
        }
        else if (what.equals("hbsp")) {
          setMemoryHblankStopPosition(value);
        }
        else if (what.equals("hbstd")) {
          setDisplayHblankStartPosition(value);
        }
        else if (what.equals("hbspd")) {
          setDisplayHblankStopPosition(value);
        }
        else if (what.equals("vsst")) {
          setVSyncStartPosition(value);
        }
        else if (what.equals("vssp")) {
          setVSyncStopPosition(value);
        }
        else if (what.equals("vbst")) {
          setMemoryVblankStartPosition(value);
        }
        else if (what.equals("vbsp")) {
          setMemoryVblankStopPosition(value);
        }
        else if (what.equals("vbstd")) {
          setDisplayVblankStartPosition(value);
        }
        else if (what.equals("vbspd")) {
          setDisplayVblankStopPosition(value);
        }
        else if (what.equals("sog")) {
          setAndUpdateSogLevel(value);
        }
      }
      else {
        SerialM.println("abort");
      }
    }
    break;
    case 'x':
    {
      uint16_t if_hblank_scale_stop = GBS::IF_HBIN_SP::read();
      GBS::IF_HBIN_SP::write(if_hblank_scale_stop + 1);
      SerialM.print("1_26: "); SerialM.println(if_hblank_scale_stop + 1);
    }
    break;
    default:
      SerialM.println("error");
      inputStage = 0;
      uint8_t temp = 0xff;
      while (Serial.available() && temp > 0) {
        Serial.read(); // eat extra characters
        temp--;
        delay(1); // wifi stack
      }
      break;
    }
    // a web ui or terminal command has finished. good idea to reset sync lock timer
    // important if the command was to change presets, possibly others
    lastVsyncLock = millis();
  }
  globalCommand = 0; // in case the web server had this set

  // run FrameTimeLock if enabled
  if (uopt->enableFrameTimeLock && rto->sourceDisconnected == false && rto->syncLockEnabled && 
    rto->syncWatcherEnabled && FrameSync::ready() && millis() - lastVsyncLock > FrameSyncAttrs::lockInterval) {
    
    uint8_t debugRegSPBackup = GBS::TEST_BUS_SP_SEL::read();
    if (debugRegSPBackup != 0x0f) GBS::TEST_BUS_SP_SEL::write(0x0f);
    lastVsyncLock = millis(); // todo: before actually doing it?
    if (!FrameSync::run(uopt->frameTimeLockMethod)) {
      if (rto->syncLockFailIgnore-- == 0) {
        FrameSync::reset(); // in case run() failed because we lost a sync signal
      }
    }
    else if (rto->syncLockFailIgnore > 0) {
      rto->syncLockFailIgnore = 2;
    }
    GBS::TEST_BUS_SP_SEL::write(debugRegSPBackup);
  }

  if (rto->printInfos == true) { // information mode
    static uint8_t runningNumber = 0;
    uint8_t lockCounter = 0;
    static uint8_t buffer[99] = { 0 };

    uint8_t stat16 = GBS::STATUS_16::read();
    uint8_t stat5 = GBS::STATUS_05::read();
    uint8_t video_mode = getVideoMode();
    uint8_t adc_gain = (GBS::ADC_RGCTRL::read() + GBS::ADC_GGCTRL::read() + GBS::ADC_BGCTRL::read()) / 3;
    uint16_t HPERIOD_IF = GBS::HPERIOD_IF::read();
    uint16_t VPERIOD_IF = GBS::VPERIOD_IF::read();
    uint16_t TEST_BUS = GBS::TEST_BUS::read();
    uint16_t STATUS_SYNC_PROC_HTOTAL = GBS::STATUS_SYNC_PROC_HTOTAL::read();
    uint16_t STATUS_SYNC_PROC_VTOTAL = GBS::STATUS_SYNC_PROC_VTOTAL::read();
    uint16_t STATUS_SYNC_PROC_HLOW_LEN = GBS::STATUS_SYNC_PROC_HLOW_LEN::read();
    boolean STATUS_MISC_PLL648_LOCK = GBS::STATUS_MISC_PLL648_LOCK::read();
    runningNumber++;
    if (runningNumber == 99) { runningNumber = 0; }
    buffer[runningNumber] = GBS::STATUS_MISC_PLLAD_LOCK::read();
    for (uint8_t i = 0; i < 99; i++) {
      lockCounter += buffer[i];
    }
    //lockDisplay /= 10;
    //if (STATUS_MISC_PLLAD_LOCK) { runningLocks++; }
    
    String dbg = TEST_BUS < 0x10 ? "000" + String(TEST_BUS, HEX) : TEST_BUS < 0x100 ? "00" + String(TEST_BUS, HEX) : TEST_BUS < 0x1000 ? "0" + String(TEST_BUS, HEX) : String(TEST_BUS, HEX);
    String hv_stable = GBS::STATUS_IF_HVT_OK::read() == 1 ? " " : "!";
    String interlace_progressive = GBS::STATUS_IF_INP_INT::read() == 1 ? " laced" : " progr";
    String hpw = STATUS_SYNC_PROC_HLOW_LEN < 100 ? "00" + String(STATUS_SYNC_PROC_HLOW_LEN) : STATUS_SYNC_PROC_HLOW_LEN < 1000 ? "0" + String(STATUS_SYNC_PROC_HLOW_LEN) : String(STATUS_SYNC_PROC_HLOW_LEN);
    String lockDisplay = lockCounter < 10 ? "0" + String(lockCounter) : String(lockCounter);

    String output = "h:" + String(HPERIOD_IF) + " " + "v:" + String(VPERIOD_IF) + " PLL" +
      (STATUS_MISC_PLL648_LOCK ? "." : "x") + lockDisplay + " adc:" + String(adc_gain, HEX) +
      " stat:" + String(stat16, HEX) + String(".") + String(stat5, HEX) +
      " deb:" + dbg + " m:" + String(video_mode) + " ht:" + String(STATUS_SYNC_PROC_HTOTAL) +
      " vt:" + String(STATUS_SYNC_PROC_VTOTAL) + " hpw:" + hpw +
      interlace_progressive + hv_stable
#if defined(ESP8266)
      +String(" WiFi:") + String(WiFi.RSSI())
#endif
      ;

    SerialM.println(output);
  } // end information mode

  // syncwatcher polls SP status. when necessary, initiates adjustments or preset changes
  if (rto->sourceDisconnected == false && rto->syncWatcherEnabled == true && (millis() - lastTimeSyncWatcher) > 40) {
    uint8_t debugRegBackup = GBS::TEST_BUS_SP_SEL::read();
    if (debugRegBackup != 0x0f) GBS::TEST_BUS_SP_SEL::write(0x0f);

    uint8_t newVideoMode = getVideoMode();
    if (!getSyncStable() || newVideoMode == 0) {
      noSyncCounter++;
      LEDOFF;
      if (noSyncCounter == 1 && rto->printInfos == false) { SerialM.print("."); }
      if (noSyncCounter == 6) { GBS::SP_NO_CLAMP_REG::write(1); rto->clampWasTurnedOff = 1; }
      if (noSyncCounter == 20) { SerialM.println("!"); }
      lastVsyncLock = millis(); // delay sync locking
    }
    else LEDON;

    // if format changed to valid
    if ((newVideoMode != 0 && newVideoMode != rto->videoStandardInput && getSyncStable()) ||
      (newVideoMode != 0 && rto->videoStandardInput == 0 /*&& getSyncPresent()*/)) {
      noSyncCounter = 0;
      uint8_t test = 10;
      uint8_t changeToPreset = newVideoMode;
      uint8_t signalInputChangeCounter = 0;

      // this first test is necessary with "dirty" sync (CVid)
      while (--test > 0) { // what's the new preset?
        delay(2);
        newVideoMode = getVideoMode();
        if (changeToPreset == newVideoMode) {
          signalInputChangeCounter++;
        }
      }
      if (signalInputChangeCounter >= 8) { // video mode has changed
        SerialM.println("New Input");
        uint8_t timeout = 255;
        while (newVideoMode == 0 && --timeout > 0) {
          newVideoMode = getVideoMode();
          delay(1); // rarely needed but better than not
        }
        if (timeout > 0) {
          boolean wantSyncLockMode = rto->outModeSynclock;
          disableVDS();
          applyPresets(newVideoMode);
          if (wantSyncLockMode) {
            rto->outModeSynclock = 0; // passThroughWithIFModeSwitch() will set it to 1
            passThroughWithIFModeSwitch();
          }
          debugRegBackup = 0;
          delay(20); // only a brief delay
          rto->clampWasTurnedOff = 1; // make sure syncwatcher enables clamp
          lastVsyncLock = millis(); // not all aspects stable enough yet
        }
        else {
          SerialM.println(" .. lost");
        }
        noSyncCounter = 0;
      }
    }
    else if (getSyncStable() && newVideoMode != 0) { // last used mode reappeared / stable again
      noSyncCounter = 0;
      if (rto->clampWasTurnedOff) { GBS::SP_NO_CLAMP_REG::write(0); rto->clampWasTurnedOff = 0; }
      if (rto->deinterlacerWasTurnedOff) {
        SerialM.print("ok ");
        //updateCoastPosition();
        if ((rto->currentSyncProcessorMode > 0) && (newVideoMode <= 2)) { // if HD mode loaded and source is SD
          SerialM.println("go SD (fixme)");
          GBS::PLLAD_FS::write(0); // low high
          delay(100);
        }
        else if ((rto->currentSyncProcessorMode == 0) && (newVideoMode > 2)) { // if HD mode loaded and source is SD
          SerialM.println("go HD (fixme)");
          GBS::PLLAD_FS::write(1); // gain high
          delay(100);
        }
        latchPLLAD();
        //togglePhaseAdjustUnits();
        setPhaseSP(); setPhaseADC();
        enableDeinterlacer();
        FrameSync::reset(); // corner case: source changed timings inbetween power cycles. won't affect display if timings are the same
        delay(60);
      }
    }

    if (noSyncCounter >= 20) { // clamp is already off, attempt something
      if (noSyncCounter == 60) { // signal lost
        disableDeinterlacer();
      }
      // todo: optimize optimizeSogLevel()
      //if (noSyncCounter % 70 == 0) { optimizeSogLevel(); }
      if (noSyncCounter % 20 == 0) {
        if (rto->videoStandardInput != 15) {
          static boolean toggle = 0;
          if (toggle) {
            toggle = 0;
            SerialM.print("HD? ");
            GBS::SP_PRE_COAST::write(0x05);
            GBS::SP_POST_COAST::write(0x05);
          }
          else {
            toggle = 1;
            SerialM.print("SD? ");
            GBS::SP_PRE_COAST::write(0x0a);
            GBS::SP_POST_COAST::write(0x11);
          }
          resetModeDetect();
          delay(30);
        }
        else { // is in RGBHV
          if (GBS::STATUS_SYNC_PROC_VTOTAL::read() > 608) { // crude
            GBS::PLLAD_FS::write(1); // high gain
            GBS::PLLAD_ICP::write(6);
            GBS::PLLAD_MD::write(1349);
            delay(10);
          }
          else {
            GBS::PLLAD_FS::write(0);
            GBS::PLLAD_ICP::write(6);
            GBS::PLLAD_MD::write(1856);
            delay(10);
          }
          latchPLLAD();
          //togglePhaseAdjustUnits();
          setPhaseSP(); setPhaseADC();
          SerialM.print("> "); SerialM.println(GBS::PLLAD_MD::read());
        }
      }

      // couldn't recover, source is lost
      // restore initial conditions and move to input detect
      if (noSyncCounter >= 220) {
        zeroAll();
        setResetParameters();
        loadPresetMdSection(); // fills 1_60 to 1_83 (mode detect segment, mostly static)
        setAdcParameters();
        setSpParameters();
        setAndUpdateSogLevel(8);
        noSyncCounter = 0;
        debugRegBackup = 0;
        rto->sourceDisconnected = true;
      }
    }

    if (debugRegBackup != 0) GBS::TEST_BUS_SP_SEL::write(debugRegBackup);
    lastTimeSyncWatcher = millis();
  }

  //static unsigned long lastTimeMinimalStatus = millis();
  //if (!rto->syncWatcherEnabled && (millis() - lastTimeMinimalStatus) > 20) { // just some status
  //  if (getVideoMode() == 0) { LEDOFF; }
  //  else { LEDON; }
  //  lastTimeMinimalStatus = millis();
  //}

  // frame sync + besthtotal init routine. last step of applying a preset
  // (this only runs if !FrameSync::ready(), ie manual retiming, preset load, etc)
  if (!FrameSync::ready() && noSyncCounter == 0 && rto->sourceDisconnected == false  && rto->syncWatcherEnabled == true
    && rto->syncLockEnabled == true && rto->videoStandardInput != 0 && rto->videoStandardInput != 15 
    && rto->outModeSynclock != 1 && !rto->deinterlacerWasTurnedOff) {
    //lastVsyncLock check removed

    // make very sure sync is stable!
    if (getSyncStable() && getSyncStable() && getSyncStable()) {
      uint8_t debugRegSPBackup = GBS::TEST_BUS_SP_SEL::read();
      if (debugRegSPBackup != 0x0f) GBS::TEST_BUS_SP_SEL::write(0x0f);
      uint16_t bestHTotal = FrameSync::init();
      if (bestHTotal > 0) {
        applyBestHTotal(bestHTotal);
        rto->syncLockFailIgnore = 2;
      }
      else if (rto->syncLockFailIgnore-- == 0) {
        // frame time lock failed, most likely due to missing wires
        rto->syncLockEnabled = false;
        SerialM.println("lock failed, check debug wire!");
      }
      GBS::TEST_BUS_SP_SEL::write(debugRegSPBackup);
    }
    else {
      SerialM.println("dl");
      //lastVsyncLock = millis(); // else delay until stable // see above
    }

    if (GBS::PAD_SYNC_OUT_ENZ::read()) { // if 1 > sync still off
      GBS::PAD_SYNC_OUT_ENZ::write(0); // output sync > display goes on
    }
    lastVsyncLock = millis();
    //delay(100);
  }

  // need to reset ModeDetect shortly after loading a new preset
  // if the source has eq pulses, the image position needs correcting
  if (rto->applyPresetDone == 1 && (millis() - rto->applyPresetDoneTime > 250)) {
    // 2 modes: 
    // t5t55t7 on = auto clamp > start and stop pos should be the same
    // t5t55t7 off = manual clamp > start and stop pos variable
    updateClampPosition();
    // SD modes // this doesn't work too well
    //if (rto->videoStandardInput <= 2) {
    //  uint16_t vLinexMin = rto->videoStandardInput == 1 ? 259 : 308;
    //  uint16_t vLinexMax = rto->videoStandardInput == 1 ? 274 : 328;
    //  rto->sourceVLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
    //  unsigned long timeOutStart = millis();
    //  while ((rto->sourceVLines <= vLinexMin || rto->sourceVLines >= vLinexMax) && (millis() - timeOutStart < 2500)) {
    //    rto->sourceVLines = GBS::STATUS_SYNC_PROC_VTOTAL::read();
    //    delay(1);
    //  }
    //  SerialM.print("vlines: "); SerialM.println(rto->sourceVLines);
    //  if (rto->sourceVLines > 263 && rto->sourceVLines <= 274) {
    //    GBS::SP_SDCS_VSST_REG_L::write(12); // 5_3f
    //    GBS::SP_SDCS_VSSP_REG_L::write(21); // 5_40
    //    // test! also lower pre coast
    //    GBS::SP_PRE_COAST::write(3);
    //    delay(50);
    //  }
    //}

    if (rto->clampWasTurnedOff) { GBS::SP_NO_CLAMP_REG::write(0); rto->clampWasTurnedOff = 0; delay(1); }
    rto->applyPresetDone = 0;

    // todo: why is auto clamp failing unless this is done?
    resetModeDetect();
    delay(300);
  }

  if (rto->syncWatcherEnabled == true && rto->sourceDisconnected == true) {
    if ((millis() - lastTimeSourceCheck) > 80 && getVideoMode() > 0) {
      detectAndSwitchToActiveInput();
      lastTimeSourceCheck = millis();

    }
    else if ((millis() - lastTimeSourceCheck) > 750) {
      detectAndSwitchToActiveInput(); // source is off; keep looking for new input
      lastTimeSourceCheck = millis();
    }
  }

#if defined(ESP8266) // no more space on ATmega
  // run auto ADC gain feature (if enabled)
  if (rto->syncWatcherEnabled && uopt->enableAutoGain == 1 && !rto->sourceDisconnected 
    && getVideoMode() != 0 && getSyncStable()) {
    uint8_t debugRegBackup = 0, debugPinBackup = 0;
    debugPinBackup = GBS::PAD_BOUT_EN::read();
    debugRegBackup = GBS::TEST_BUS_SEL::read();
    GBS::PAD_BOUT_EN::write(0); // disable output to pin for test
    GBS::TEST_BUS_SEL::write(0xb); // decimation
    GBS::DEC_TEST_ENABLE::write(1);
    doAutoGain();
    GBS::DEC_TEST_ENABLE::write(0);
    GBS::TEST_BUS_SEL::write(debugRegBackup);
    GBS::PAD_BOUT_EN::write(debugPinBackup); // debug output pin back on
  }
#endif
}

#if defined(ESP8266)

void handleRoot() {
  server.send_P(200, "text/html", HTML); // send_P removes the need for FPSTR(HTML)
  server.client().stop(); // not sure
}

void handleType1Command() {
  server.send(200);
  server.client().stop(); // not sure
  if (server.hasArg("plain")) {
    globalCommand = server.arg("plain").charAt(0);
  }
}

void handleType2Command() {
  server.send(200);
  server.client().stop(); // not sure
  if (server.hasArg("plain")) {
    char argument = server.arg("plain").charAt(0);
    switch (argument) {
    case '0':
      uopt->presetPreference = 0; // normal
      saveUserPrefs();
      break;
    case '1':
      uopt->presetPreference = 1; // prefer fb clock
      saveUserPrefs();
      break;
    case '2':
      uopt->presetPreference = 4; // prefer 1280x1024 preset
      saveUserPrefs();
      break;
    case '3':  // load custom preset
    {
      if (rto->videoStandardInput == 0) SerialM.println("no input detected, aborting action");
      else {
        const uint8_t* preset = loadPresetFromSPIFFS(rto->videoStandardInput); // load for current video mode
        writeProgramArrayNew(preset);
        doPostPresetLoadSteps();
      }
    }
    break;
    case '4':
      if (rto->videoStandardInput == 0) SerialM.println("no input detected, aborting action");
      else savePresetToSPIFFS();
      break;
    case '5':
      //Frame Time Lock ON
      uopt->enableFrameTimeLock = 1;
      saveUserPrefs();
      break;
    case '6':
      //Frame Time Lock OFF
      uopt->enableFrameTimeLock = 0;
      saveUserPrefs();
      break;
    case '7':
    {
      // scanline toggle
      uopt->enableAutoGain = 0; // incompatible
      uint8_t reg;
      writeOneByte(0xF0, 2);
      readFromRegister(0x16, 1, &reg);
      if ((reg & 0x80) == 0x80) {
        writeOneByte(0x16, reg ^ (1 << 7));
        GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() - 0x20);
        GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() - 0x20);
        GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() - 0x20);
        writeOneByte(0xF0, 3);
        writeOneByte(0x35, 0xd0); // more luma gain
        writeOneByte(0xF0, 2);
        writeOneByte(0x27, 0x28); // set up VIIR filter (no need to undo later)
        writeOneByte(0x26, 0x00);
      }
      else {
        writeOneByte(0x16, reg ^ (1 << 7));
        GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() + 0x20);
        GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() + 0x20);
        GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() + 0x20);
        writeOneByte(0xF0, 3);
        writeOneByte(0x35, 0x80);
        writeOneByte(0xF0, 2);
        writeOneByte(0x26, 0x40); // disables VIIR filter
      }
    }
    break;
    case '9':
      uopt->presetPreference = 3; // prefer 720p preset
      saveUserPrefs();
      break;
    case 'a':
      // restart ESP MCU (due to an SDK bug, this does not work reliably after programming. It needs a power cycle or reset button push first.)
      SerialM.print("Attempting to restart MCU. If it hangs, reset manually!"); SerialM.println("\n");
      ESP.restart();
      break;
    case 'b':
      uopt->presetGroup = 0;
      uopt->presetPreference = 2; // custom
      saveUserPrefs();
      break;
    case 'c':
      uopt->presetGroup = 1;
      uopt->presetPreference = 2;
      saveUserPrefs();
      break;
    case 'd':
      uopt->presetGroup = 2;
      uopt->presetPreference = 2;
      saveUserPrefs();
      break;
    case 'j':
      uopt->presetGroup = 3;
      uopt->presetPreference = 2;
      saveUserPrefs();
      break;
    case 'k':
      uopt->presetGroup = 4;
      uopt->presetPreference = 2;
      saveUserPrefs();
      break;
    case 'e': // print files on spiffs
    {
      Dir dir = SPIFFS.openDir("/");
      while (dir.next()) {
        SerialM.print(dir.fileName()); SerialM.print(" "); SerialM.println(dir.fileSize());
        delay(1); // wifi stack
      }
      ////
      File f = SPIFFS.open("/userprefs.txt", "r");
      if (!f) {
        SerialM.println("userprefs open failed");
      }
      else {
        char result[4];
        result[0] = f.read(); result[0] -= '0'; // file streams with their chars..
        SerialM.print("presetPreference = "); SerialM.println((uint8_t)result[0]);
        result[1] = f.read(); result[1] -= '0';
        SerialM.print("FrameTime Lock = "); SerialM.println((uint8_t)result[1]);
        result[2] = f.read(); result[2] -= '0';
        SerialM.print("presetGroup = "); SerialM.println((uint8_t)result[2]);
        result[3] = f.read(); result[3] -= '0';
        SerialM.print("frameTimeLockMethod = "); SerialM.println((uint8_t)result[3]);
        f.close();
      }
    }
    break;
    case 'f':
    {
      // load 1280x960 preset via webui
      uint8_t videoMode = getVideoMode();
      if (videoMode == 0) videoMode = rto->videoStandardInput; // last known good as fallback
      uint8_t backup = uopt->presetPreference;
      uopt->presetPreference = 0; // override RAM copy of presetPreference for applyPresets
      rto->videoStandardInput = 0; // force hard reset
      applyPresets(videoMode);
      uopt->presetPreference = backup;
    }
    break;
    case 'g':
    {
      // load 1280x720 preset via webui
      uint8_t videoMode = getVideoMode();
      if (videoMode == 0) videoMode = rto->videoStandardInput;
      uint8_t backup = uopt->presetPreference;
      uopt->presetPreference = 3;
      rto->videoStandardInput = 0; // force hard reset
      applyPresets(videoMode);
      uopt->presetPreference = backup;
    }
    break;
    case 'h':
    {
      // load 640x480 preset via webui
      uint8_t videoMode = getVideoMode();
      if (videoMode == 0) videoMode = rto->videoStandardInput;
      uint8_t backup = uopt->presetPreference;
      uopt->presetPreference = 1;
      rto->videoStandardInput = 0; // force hard reset
      applyPresets(videoMode);
      uopt->presetPreference = backup;
    }
    break;
    case 'p':
    {
      // load 1280x1024
      uint8_t videoMode = getVideoMode();
      if (videoMode == 0) videoMode = rto->videoStandardInput;
      uint8_t backup = uopt->presetPreference;
      uopt->presetPreference = 4;
      rto->videoStandardInput = 0; // force hard reset
      applyPresets(videoMode);
      uopt->presetPreference = backup;
    }
    break;
    case 'i':
      // toggle active frametime lock method
      if (uopt->frameTimeLockMethod == 0) uopt->frameTimeLockMethod = 1;
      else if (uopt->frameTimeLockMethod == 1) uopt->frameTimeLockMethod = 0;
      saveUserPrefs();
      break;
    case 'l':
      // cycle through available SDRAM clocks
    {
      uint8_t PLL_MS = GBS::PLL_MS::read();
      uint8_t memClock = 0;
      PLL_MS++; PLL_MS &= 0x7;
      switch (PLL_MS) {
      case 0: memClock = 108; break;
      case 1: memClock = 81; break; // goes well with 4_2C = 0x14, 4_2D = 0x27
      case 2: memClock = 10; break; //feedback clock
      case 3: memClock = 162; break;
      case 4: memClock = 144; break;
      case 5: memClock = 185; break;
      case 6: memClock = 216; break;
      case 7: memClock = 129; break;
      default: break;
      }
      GBS::PLL_MS::write(PLL_MS);
      ResetSDRAM();
      if (memClock != 10) {
        SerialM.print("SDRAM clock: "); SerialM.print(memClock); SerialM.println("Mhz");
      }
      else {
        SerialM.print("SDRAM clock: "); SerialM.println("Feedback clock (default)");
      }
    }
    break;
    case 'm':
      // DCTI (pixel edges slope enhancement)
      if (GBS::VDS_UV_STEP_BYPS::read() == 1) {
        GBS::VDS_UV_STEP_BYPS::write(0);
        // VDS_TAP6_BYPS (S3_24, 3) no longer enabled by default
        /*if (GBS::VDS_TAP6_BYPS::read() == 1) {
          GBS::VDS_TAP6_BYPS::write(0); // no good way to store this change for later reversal
          GBS::VDS_0X2A_RESERVED_2BITS::write(1); // so use this trick to detect it later
          }*/
        SerialM.println("DCTI on");
      }
      else {
        GBS::VDS_UV_STEP_BYPS::write(1);
        /*if (GBS::VDS_0X2A_RESERVED_2BITS::read() == 1) {
          GBS::VDS_TAP6_BYPS::write(1);
          GBS::VDS_0X2A_RESERVED_2BITS::write(0);
          }*/
        SerialM.println("DCTI off");
      }
      break;
    case 'n':
      SerialM.print("ADC gain++ : ");
      GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() - 1);
      GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() - 1);
      GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() - 1);
      SerialM.println(GBS::ADC_RGCTRL::read(), HEX);
      break;
    case 'o':
      SerialM.print("ADC gain-- : ");
      GBS::ADC_RGCTRL::write(GBS::ADC_RGCTRL::read() + 1);
      GBS::ADC_GGCTRL::write(GBS::ADC_GGCTRL::read() + 1);
      GBS::ADC_BGCTRL::write(GBS::ADC_BGCTRL::read() + 1);
      SerialM.println(GBS::ADC_RGCTRL::read(), HEX);
      break;
    case 'A':
      //GBS::VDS_DIS_HB_ST::write(GBS::VDS_DIS_HB_ST::read() - 4);
      GBS::VDS_DIS_HB_SP::write(GBS::VDS_DIS_HB_SP::read() + 4);
      break;
    case 'B':
      //GBS::VDS_DIS_HB_ST::write(GBS::VDS_DIS_HB_ST::read() + 4);
      GBS::VDS_DIS_HB_SP::write(GBS::VDS_DIS_HB_SP::read() - 4);
      break;
    case 'C':
      GBS::VDS_DIS_VB_ST::write(GBS::VDS_DIS_VB_ST::read() - 2);
      GBS::VDS_DIS_VB_SP::write(GBS::VDS_DIS_VB_SP::read() + 2);
      break;
    case 'D':
      GBS::VDS_DIS_VB_ST::write(GBS::VDS_DIS_VB_ST::read() + 2);
      GBS::VDS_DIS_VB_SP::write(GBS::VDS_DIS_VB_SP::read() - 2);
      break;
    default:
      break;
    }
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght);

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {
  switch (type) {
  case WStype_DISCONNECTED:             // if the websocket is disconnected
    Serial.println("Websocket Disconnected");
    //rto->webSocketConnected = false;
    break;
  case WStype_CONNECTED:               // if a new websocket connection is established
    SerialM.println("Websocket Connected");
    //rto->webSocketConnected = true;
    break;
  case WStype_TEXT:                     // if new text data is received
    //SerialM.printf("[%u] get Text (length: %d): %s\n", num, lenght, payload);
    break;
  default:
    break;
  }
}

void start_webserver()
{
  //WiFi.disconnect(); // test captive portal by forgetting wifi credentials
  persWM.setApCredentials(ap_ssid, ap_password);
  persWM.onConnect([]() {
    WiFi.hostname("gbscontrol");
    SerialM.print("local IP: ");
    SerialM.println(WiFi.localIP());
    SerialM.print("hostname: "); SerialM.println(WiFi.hostname());
  });
  persWM.onAp([]() {
    WiFi.hostname("gbscontrol");
    SerialM.print("AP MODE: "); SerialM.println("connect to wifi network called gbscontrol with password qqqqqqqq");
    //SerialM.println(persWM.getApSsid()); // crash with exception
    //SerialM.print("hostname: "); SerialM.println(WiFi.hostname());
  });

  server.on("/", handleRoot);
  server.on("/serial_", handleType1Command);
  server.on("/user_", handleType2Command);

  persWM.setConnectNonBlock(true);
  persWM.begin(); // WiFiManager with captive portal
  MDNS.begin("gbscontrol"); // respnd to MDNS request for gbscontrol.local
  server.begin(); // Webserver for the site
  webSocket.begin();  // Websocket for interaction
  webSocket.onEvent(webSocketEvent);
}

void initUpdateOTA() {
  ArduinoOTA.setHostname("GBS OTA");

  // ArduinoOTA.setPassword("admin");
  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    SPIFFS.end();
    SerialM.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    SerialM.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    SerialM.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    SerialM.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) SerialM.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) SerialM.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) SerialM.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) SerialM.println("Receive Failed");
    else if (error == OTA_END_ERROR) SerialM.println("End Failed");
  });
  ArduinoOTA.begin();
}

// sets every element of str to 0 (clears array)
void StrClear(char *str, uint16_t length)
{
  for (int i = 0; i < length; i++) {
    str[i] = 0;
  }
}

const uint8_t* loadPresetFromSPIFFS(byte forVideoMode) {
  static uint8_t preset[496];
  String s = "";
  char group = '0';
  File f;

  f = SPIFFS.open("/userprefs.txt", "r");
  if (f) {
    SerialM.println("userprefs.txt opened");
    char result[3];
    result[0] = f.read(); // todo: move file cursor manually
    result[1] = f.read();
    result[2] = f.read();

    f.close();
    if ((uint8_t)(result[2] - '0') < 10) group = result[2]; // otherwise not stored on spiffs
    SerialM.print("loading from presetGroup "); SerialM.println((uint8_t)(group - '0')); // custom preset group (console)
  }
  else {
    // file not found, we don't know what preset to load
    SerialM.println("please select a preset group first!");
    if (forVideoMode == 2 || forVideoMode == 4) return pal_240p;
    else return ntsc_240p;
  }

  if (forVideoMode == 1) {
    f = SPIFFS.open("/preset_ntsc." + String(group), "r");
  }
  else if (forVideoMode == 2) {
    f = SPIFFS.open("/preset_pal." + String(group), "r");
  }
  else if (forVideoMode == 3) {
    f = SPIFFS.open("/preset_ntsc_480p." + String(group), "r");
  }
  else if (forVideoMode == 4) {
    f = SPIFFS.open("/preset_pal_576p." + String(group), "r");
  }
  else if (forVideoMode == 5) {
    f = SPIFFS.open("/preset_ntsc_720p." + String(group), "r");
  }
  else if (forVideoMode == 6) {
    f = SPIFFS.open("/preset_ntsc_1080p." + String(group), "r");
  }

  if (!f) {
    SerialM.println("open preset file failed");
    if (forVideoMode == 2 || forVideoMode == 4) return pal_240p;
    else return ntsc_240p;
  }
  else {
    SerialM.println("preset file open ok: ");
    SerialM.println(f.name());
    s = f.readStringUntil('}');
    f.close();
  }

  char *tmp;
  uint16_t i = 0;
  tmp = strtok(&s[0], ",");
  while (tmp) {
    preset[i++] = (uint8_t)atoi(tmp);
    tmp = strtok(NULL, ",");
    yield(); // wifi stack
  }

  return preset;
}

void savePresetToSPIFFS() {
  uint8_t readout = 0;
  File f;
  char group = '0';

  // first figure out if the user has set a preferenced group
  f = SPIFFS.open("/userprefs.txt", "r");
  if (f) {
    char result[3];
    result[0] = f.read(); // todo: move file cursor manually
    result[1] = f.read();
    result[2] = f.read();

    f.close();
    group = result[2];
    SerialM.print("saving to presetGroup "); SerialM.println(result[2]); // custom preset group (console)
  }
  else {
    // file not found, we don't know where to save this preset
    SerialM.println("please select a preset group first!");
    return;
  }

  if (rto->videoStandardInput == 1) {
    f = SPIFFS.open("/preset_ntsc." + String(group), "w");
  }
  else if (rto->videoStandardInput == 2) {
    f = SPIFFS.open("/preset_pal." + String(group), "w");
  }
  else if (rto->videoStandardInput == 3) {
    f = SPIFFS.open("/preset_ntsc_480p." + String(group), "w");
  }
  else if (rto->videoStandardInput == 4) {
    f = SPIFFS.open("/preset_pal_576p." + String(group), "w");
  }
  else if (rto->videoStandardInput == 5) {
    f = SPIFFS.open("/preset_ntsc_720p." + String(group), "w");
  }
  else if (rto->videoStandardInput == 6) {
    f = SPIFFS.open("/preset_ntsc_1080p." + String(group), "w");
  }

  if (!f) {
    SerialM.println("open preset file failed");
  }
  else {
    SerialM.println("preset file open ok");

    GBS::ADC_0X00_RESERVED_5::write(1); // use one reserved bit to mark this as a custom preset

    for (int i = 0; i <= 5; i++) {
      writeOneByte(0xF0, i);
      switch (i) {
      case 0:
        for (int x = 0x40; x <= 0x5F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        for (int x = 0x90; x <= 0x9F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 1:
        for (int x = 0x0; x <= 0x2F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 2:
        for (int x = 0x0; x <= 0x3F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 3:
        for (int x = 0x0; x <= 0x7F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 4:
        for (int x = 0x0; x <= 0x5F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      case 5:
        for (int x = 0x0; x <= 0x6F; x++) {
          readFromRegister(x, 1, &readout);
          f.print(readout); f.println(",");
        }
        break;
      }
    }
    f.println("};");
    SerialM.print("preset saved as: ");
    SerialM.println(f.name());
    f.close();
  }
}

void saveUserPrefs() {
  File f = SPIFFS.open("/userprefs.txt", "w");
  if (!f) {
    SerialM.println("saving preferences failed");
    return;
  }
  f.write(uopt->presetPreference + '0');
  f.write(uopt->enableFrameTimeLock + '0');
  f.write(uopt->presetGroup + '0');
  f.write(uopt->frameTimeLockMethod + '0');
  SerialM.println("userprefs saved: ");
  f.close();

  // print results
  f = SPIFFS.open("/userprefs.txt", "r");
  if (!f) {
    SerialM.println("userprefs open failed");
  }
  else {
    char result[4];
    result[0] = f.read(); result[0] -= '0'; // file streams with their chars..
    SerialM.print("  presetPreference = "); SerialM.println((uint8_t)result[0]);
    result[1] = f.read(); result[1] -= '0';
    SerialM.print("  FrameTime Lock = "); SerialM.println((uint8_t)result[1]);
    result[2] = f.read(); result[2] -= '0';
    SerialM.print("  presetGroup = "); SerialM.println((uint8_t)result[2]);
    result[3] = f.read(); result[3] -= '0';
    SerialM.print("  frameTimeLockMethod = "); SerialM.println((uint8_t)result[3]);
    f.close();
  }
}

#endif
