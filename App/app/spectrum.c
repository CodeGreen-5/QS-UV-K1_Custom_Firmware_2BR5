/* Copyright 2023 fagci
 * https://github.com/fagci
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */
#include "app/spectrum.h"
#include "am_fix.h"
#include "audio.h"
#include "misc.h"

#ifdef ENABLE_SCAN_RANGES
#include "chFrScanner.h"
#endif

#include "driver/backlight.h"
#include "frequencies.h"
#include "ui/helper.h"
#include "ui/main.h"

#ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
#include "screenshot.h"
#endif

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
#include "driver/py25q16.h"
#endif

struct FrequencyBandInfo
{
    uint32_t lower;
    uint32_t upper;
    uint32_t middle;
};

#define F_MIN frequencyBandTable[0].lower
#define F_MAX frequencyBandTable[BAND_N_ELEM - 1].upper

const uint16_t RSSI_MAX_VALUE = 65535;

static uint32_t initialFreq;
static char String[32];

static bool isInitialized = false;
bool isListening = true;
bool monitorMode = false;
bool redrawStatus = true;
bool redrawScreen = false;
bool newScanStart = true;
bool preventKeypress = true;
bool audioState = true;
bool lockAGC = false;

State currentState = SPECTRUM, previousState = SPECTRUM;

PeakInfo peak;
ScanInfo scanInfo;
static KeyboardState kbd = {KEY_INVALID, KEY_INVALID, 0};

#ifdef ENABLE_SCAN_RANGES
static uint16_t blacklistFreqs[15];
static uint8_t blacklistFreqsIdx;
#endif

const char *bwOptions[] = {"25", "12.5", "6.25"};
const uint8_t modulationTypeTuneSteps[] = {100, 50, 10};
const uint8_t modTypeReg47Values[] = {1, 7, 5};

SpectrumSettings settings = {.stepsCount = STEPS_64,
                             .scanStepIndex = S_STEP_25_0kHz,
                             .frequencyChangeStep = 80000,
                             .scanDelay = 3200,
                             .rssiTriggerLevel = 150,
                             .backlightState = true,
                             .bw = BK4819_FILTER_BW_WIDE,
                             .listenBw = BK4819_FILTER_BW_WIDE,
                             .modulationType = false,
                             .dbMin = -130,
                             .dbMax = -50};

uint32_t fMeasure = 0;
uint32_t currentFreq, tempFreq;
uint16_t rssiHistory[128];

// Professional spectrum enhancements: peak hold and smoothing buffers
#define SPECTRUM_PEAK_HOLD_TIME 5   // frames to hold peak values (reduced for faster fall-to-floor)
#define SPECTRUM_SMOOTH_WINDOW 3    // averaging window for adjacent bins
static uint16_t spectrum_peaks[128];
static uint8_t spectrum_peak_age[128];
static uint16_t spectrum_smoothed[128];

// Professional 4x4 Bayer waterfall definitions
#define WATERFALL_ROWS_PIXELS 16
#define WATERFALL_PAGES (WATERFALL_ROWS_PIXELS / 8)
#define WATERFALL_PAGE_START 4
#define RULER_PAGE 3

// Waterfall buffers and state - declared here for early access in ResetScanStats
static uint8_t waterfall_rows[WATERFALL_ROWS_PIXELS][16];
static uint8_t waterfall_phase = 0;       // advances 0→1→2→3→0 for temporal dithering
static uint16_t waterfall_scan_count = 0; // tracks scans for statistics

int vfo;
uint8_t freqInputIndex = 0;
uint8_t freqInputDotIndex = 0;
KEY_Code_t freqInputArr[10];
char freqInputString[11];

uint8_t menuState = 0;
uint16_t listenT = 0;

RegisterSpec registerSpecs[] = {
    {},
    {"LNAs", BK4819_REG_13, 8, 0b11, 1},
    {"LNA", BK4819_REG_13, 5, 0b111, 1},
    {"VGA", BK4819_REG_13, 0, 0b111, 1},
    {"BPF", BK4819_REG_3D, 0, 0xFFFF, 0x2aaa},
    // {"MIX", 0x13, 3, 0b11, 1}, // TODO: hidden
};

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
const int8_t LNAsOptions[] = {-19, -16, -11, 0};
const int8_t LNAOptions[] = {-24, -19, -14, -9, -6, -4, -2, 0};
const int8_t VGAOptions[] = {-33, -27, -21, -15, -9, -6, -3, 0};
const char *BPFOptions[] = {"8.46", "7.25", "6.35", "5.64", "5.08", "4.62", "4.23"};
#endif

uint16_t statuslineUpdateTimer = 0;

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
static void LoadSettings()
{
    uint8_t Data[8] = {0};
    PY25Q16_ReadBuffer(0x00c000, Data, sizeof(Data));

    settings.scanStepIndex = ((Data[3] & 0xF0) >> 4);

    if (settings.scanStepIndex > 14)
    {
        settings.scanStepIndex = S_STEP_25_0kHz;
    }

    settings.stepsCount = ((Data[3] & 0x0F) & 0b1100) >> 2;

    if (settings.stepsCount > 3)
    {
        settings.stepsCount = STEPS_64;
    }

    settings.listenBw = ((Data[3] & 0x0F) & 0b0011);

    if (settings.listenBw > 2)
    {
        settings.listenBw = BK4819_FILTER_BW_WIDE;
    }
}

static void SaveSettings()
{
    uint8_t Data[8] = {0};
    PY25Q16_ReadBuffer(0x00c000, Data, sizeof(Data));

    Data[3] = (settings.scanStepIndex << 4) | (settings.stepsCount << 2) | settings.listenBw;

    PY25Q16_WriteBuffer(0x00c000, Data, sizeof(Data), true);
}
#endif

static uint8_t DBm2S(int dbm)
{
    uint8_t i = 0;
    dbm *= -1;
    for (i = 0; i < ARRAY_SIZE(U8RssiMap); i++)
    {
        if (dbm >= U8RssiMap[i])
        {
            return i;
        }
    }
    return i;
}

static int Rssi2DBm(uint16_t rssi)
{
    return (rssi / 2) - 160 + dBmCorrTable[gRxVfo->Band];
}

static uint16_t GetRegMenuValue(uint8_t st)
{
    RegisterSpec s = registerSpecs[st];
    return (BK4819_ReadRegister(s.num) >> s.offset) & s.mask;
}

void LockAGC()
{
    RADIO_SetupAGC(settings.modulationType == MODULATION_AM, lockAGC);
    lockAGC = true;
}

static void SetRegMenuValue(uint8_t st, bool add)
{
    uint16_t v = GetRegMenuValue(st);
    RegisterSpec s = registerSpecs[st];

    if (s.num == BK4819_REG_13)
        LockAGC();

    uint16_t reg = BK4819_ReadRegister(s.num);
    if (add && v <= s.mask - s.inc)
    {
        v += s.inc;
    }
    else if (!add && v >= 0 + s.inc)
    {
        v -= s.inc;
    }
    // TODO: use max value for bits count in max value, or reset by additional
    // mask in spec
    reg &= ~(s.mask << s.offset);
    BK4819_WriteRegister(s.num, reg | (v << s.offset));
    redrawScreen = true;
}

// GUI functions

static void DrawVLine(int sy, int ey, int nx, bool fill)
{
    for (int i = sy; i <= ey; i++)
    {
        if (i < 56 && nx < 128)
        {
            PutPixel(nx, i, fill);
        }
    }
}

// Utility functions

static KEY_Code_t GetKey()
{
    KEY_Code_t btn = KEYBOARD_Poll();
    if (btn == KEY_INVALID && GPIO_IsPttPressed())
    {
        btn = KEY_PTT;
    }
    return btn;
}

static int clamp(int v, int min, int max)
{
    return v <= min ? min : (v >= max ? max : v);
}

static uint8_t my_abs(signed v) { return v > 0 ? v : -v; }

void SetState(State state)
{
    previousState = currentState;
    currentState = state;
    redrawScreen = true;
    redrawStatus = true;
}

// Radio functions

static void ToggleAFBit(bool on)
{
    uint16_t reg = BK4819_ReadRegister(BK4819_REG_47);
    reg &= ~(1 << 8);
    if (on)
        reg |= on << 8;
    BK4819_WriteRegister(BK4819_REG_47, reg);
}

static const BK4819_REGISTER_t registers_to_save[] = {
    BK4819_REG_30,
    BK4819_REG_37,
    BK4819_REG_3D,
    BK4819_REG_43,
    BK4819_REG_47,
    BK4819_REG_48,
    BK4819_REG_7E,
};

static uint16_t registers_stack[sizeof(registers_to_save)];

static void BackupRegisters(void)
{
    for (uint32_t i = 0; i < ARRAY_SIZE(registers_to_save); i++)
    {
        registers_stack[i] = BK4819_ReadRegister(registers_to_save[i]);
    }
}

static void RestoreRegisters(void)
{

    for (uint32_t i = 0; i < ARRAY_SIZE(registers_to_save); i++)
    {
        BK4819_WriteRegister(registers_to_save[i], registers_stack[i]);
    }

#ifdef ENABLE_FEAT_F4HWN
    gVfoConfigureMode = VFO_CONFIGURE;
#endif
}

static void ToggleAFDAC(bool on)
{
    uint32_t Reg = BK4819_ReadRegister(BK4819_REG_30);
    Reg &= ~(1 << 9);
    if (on)
        Reg |= (1 << 9);
    BK4819_WriteRegister(BK4819_REG_30, Reg);
}

static void SetF(uint32_t f)
{
    fMeasure = f;

    BK4819_SetFrequency(fMeasure);
    BK4819_PickRXFilterPathBasedOnFrequency(fMeasure);
    uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, reg);
}

// Spectrum related

bool IsPeakOverLevel() { return peak.rssi >= settings.rssiTriggerLevel; }

static void ResetPeak()
{
    peak.t = 0;
    peak.rssi = 0;
}

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
    static void setTailFoundInterrupt()
    {
        BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_02_CxCSS_TAIL | BK4819_REG_02_SQUELCH_FOUND);
    }

    static bool checkIfTailFound()
    {
      uint16_t interrupt_status_bits;
      // if interrupt waiting to be handled
      if(BK4819_ReadRegister(BK4819_REG_0C) & 1u) {
        // reset the interrupt
        BK4819_WriteRegister(BK4819_REG_02, 0);
        // fetch the interrupt status bits
        interrupt_status_bits = BK4819_ReadRegister(BK4819_REG_02);
        // if tail found interrupt
        if (interrupt_status_bits & BK4819_REG_02_CxCSS_TAIL)
        {
            listenT = 0;
            // disable interrupts
            BK4819_WriteRegister(BK4819_REG_3F, 0);
            // reset the interrupt
            BK4819_WriteRegister(BK4819_REG_02, 0);
            return true;
        }
      }
      return false;
    }
#endif

bool IsCenterMode() { return settings.scanStepIndex < S_STEP_2_5kHz; }
// scan step in 0.01khz
uint16_t GetScanStep() { return scanStepValues[settings.scanStepIndex]; }

uint16_t GetStepsCount()
{
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        uint32_t range = gScanRangeStop - gScanRangeStart;
        uint16_t step = GetScanStep();
        return (range / step) + 1;  // +1 to include up limit
    }
#endif
    return 128 >> settings.stepsCount;
}

#ifdef ENABLE_SCAN_RANGES
static uint16_t GetStepsCountDisplay()
{
    if (gScanRangeStart)
    {
        return (gScanRangeStop - gScanRangeStart) / GetScanStep();
    }
    return GetStepsCount();
}
#endif

uint32_t GetBW() { return GetStepsCount() * GetScanStep(); }
uint32_t GetFStart()
{
    return IsCenterMode() ? currentFreq - (GetBW() >> 1) : currentFreq;
}

uint32_t GetFEnd()
{
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        return gScanRangeStop;
    }
#endif
    return currentFreq + GetBW();
}

static void TuneToPeak()
{
    scanInfo.f = peak.f;
    scanInfo.rssi = peak.rssi;
    scanInfo.i = peak.i;
    SetF(scanInfo.f);
}

static void DeInitSpectrum()
{
    SetF(initialFreq);
    RestoreRegisters();
    isInitialized = false;
}

uint8_t GetBWRegValueForScan()
{
    return scanStepBWRegValues[settings.scanStepIndex];
}

uint16_t GetRssi()
{
    // SYSTICK_DelayUs(800);
    // testing autodelay based on Glitch value
    while ((BK4819_ReadRegister(0x63) & 0b11111111) >= 255)
    {
        SYSTICK_DelayUs(100);
    }
    uint16_t rssi = BK4819_GetRSSI();
#ifdef ENABLE_AM_FIX
    if (settings.modulationType == MODULATION_AM && gSetting_AM_fix)
        rssi += AM_fix_get_gain_diff() * 2;
#endif
    return rssi;
}

static void ToggleAudio(bool on)
{
    if (on == audioState)
    {
        return;
    }
    audioState = on;
    if (on)
    {
        AUDIO_AudioPathOn();
    }
    else
    {
        AUDIO_AudioPathOff();
    }
}

static void ToggleRX(_Bool on)
{
    #ifdef ENABLE_FEAT_F4HWN_SPECTRUM
    if (isListening == on) {
        return;
    }
    #endif
    isListening = on;

    RADIO_SetupAGC(settings.modulationType == MODULATION_AM, lockAGC);
    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, on);

    ToggleAudio(on);
    ToggleAFDAC(on);
    ToggleAFBit(on);

    if (on)
    {
    #ifdef ENABLE_FEAT_F4HWN_SPECTRUM
        listenT = 100;
        BK4819_WriteRegister(0x43, listenBWRegValues[settings.listenBw]);
        setTailFoundInterrupt();
    #else
        listenT = 1000;
        BK4819_WriteRegister(0x43, listenBWRegValues[settings.listenBw]);
    #endif
    }
    else
    {
        BK4819_WriteRegister(0x43, GetBWRegValueForScan());
    }
}

// Scan info

static void ResetScanStats()
{
    scanInfo.rssi = 0;
    scanInfo.rssiMax = 0;
    scanInfo.iPeak = 0;
    scanInfo.fPeak = 0;
    
    // Reset spectrum enhancements (peak hold and smoothing)
    memset(spectrum_peaks, 0xFF, sizeof(spectrum_peaks));  // 0xFF = RSSI_MAX_VALUE
    memset(spectrum_peak_age, 0, sizeof(spectrum_peak_age));
    
    // Reset waterfall phase and buffer to ensure synchronization with spectrum
    // This creates a clean state for the next scan cycle
    memset(waterfall_rows, 0, sizeof(waterfall_rows));
    waterfall_phase = 0;
    waterfall_scan_count = 0;
}

static void InitScan()
{
    ResetScanStats();
    scanInfo.i = 0;
    scanInfo.f = GetFStart();

    scanInfo.scanStep = GetScanStep();
    scanInfo.measurementsCount = GetStepsCount();
}

static void ResetBlacklist()
{
    for (int i = 0; i < 128; ++i)
    {
        if (rssiHistory[i] == RSSI_MAX_VALUE)
            rssiHistory[i] = 0;
    }
#ifdef ENABLE_SCAN_RANGES
    memset(blacklistFreqs, 0, sizeof(blacklistFreqs));
    blacklistFreqsIdx = 0;
#endif
}

static void RelaunchScan()
{
    InitScan();
    ResetPeak();
    ToggleRX(false);
#ifdef SPECTRUM_AUTOMATIC_SQUELCH
    settings.rssiTriggerLevel = RSSI_MAX_VALUE;
#endif
    preventKeypress = true;
    scanInfo.rssiMin = RSSI_MAX_VALUE;
}

static void UpdateScanInfo()
{
    if (scanInfo.rssi > scanInfo.rssiMax)
    {
        scanInfo.rssiMax = scanInfo.rssi;
        scanInfo.fPeak = scanInfo.f;
        scanInfo.iPeak = scanInfo.i;
    }

    if (scanInfo.rssi < scanInfo.rssiMin)
    {
        scanInfo.rssiMin = scanInfo.rssi;
        settings.dbMin = Rssi2DBm(scanInfo.rssiMin);
        redrawStatus = true;
    }
}

static void AutoTriggerLevel()
{
    if (settings.rssiTriggerLevel == RSSI_MAX_VALUE)
    {
        settings.rssiTriggerLevel = clamp(scanInfo.rssiMax + 8, 0, RSSI_MAX_VALUE);
    }
}

static void UpdatePeakInfoForce()
{
    peak.t = 0;
    peak.rssi = scanInfo.rssiMax;
    peak.f = scanInfo.fPeak;
    peak.i = scanInfo.iPeak;
    AutoTriggerLevel();
}

static void UpdatePeakInfo()
{
    if (peak.f == 0 || peak.t >= 1024 || peak.rssi < scanInfo.rssiMax)
        UpdatePeakInfoForce();
}

static void SetRssiHistory(uint16_t idx, uint16_t rssi)
{
#ifdef ENABLE_SCAN_RANGES
    if (scanInfo.measurementsCount > 128)
    {
        uint8_t i = (uint32_t)ARRAY_SIZE(rssiHistory) * 1000 / scanInfo.measurementsCount * idx / 1000;
        if (rssiHistory[i] < rssi || isListening)
            rssiHistory[i] = rssi;
        rssiHistory[(i + 1) % 128] = 0;
        return;
    }
#endif
    rssiHistory[idx] = rssi;
}

static void Measure()
{
    uint16_t rssi = scanInfo.rssi = GetRssi();
    SetRssiHistory(scanInfo.i, rssi);
}

// Update things by keypress

static uint16_t dbm2rssi(int dBm)
{
    return (dBm + 160 - dBmCorrTable[gRxVfo->Band]) * 2;
}

static void ClampRssiTriggerLevel()
{
    settings.rssiTriggerLevel =
        clamp(settings.rssiTriggerLevel, dbm2rssi(settings.dbMin),
              dbm2rssi(settings.dbMax));
}

static void UpdateRssiTriggerLevel(bool inc)
{
    if (inc)
        settings.rssiTriggerLevel += 2;
    else
        settings.rssiTriggerLevel -= 2;

    ClampRssiTriggerLevel();

    redrawScreen = true;
    redrawStatus = true;
}

static void UpdateDBMax(bool inc)
{
    if (inc && settings.dbMax < 10)
    {
        settings.dbMax += 1;
    }
    else if (!inc && settings.dbMax > settings.dbMin)
    {
        settings.dbMax -= 1;
    }
    else
    {
        return;
    }

    ClampRssiTriggerLevel();
    redrawStatus = true;
    redrawScreen = true;
    SYSTEM_DelayMs(20);
}

static void UpdateScanStep(bool inc)
{
    if (inc)
    {
        settings.scanStepIndex = settings.scanStepIndex != S_STEP_100_0kHz ? settings.scanStepIndex + 1 : 0;
    }
    else
    {
        settings.scanStepIndex = settings.scanStepIndex != 0 ? settings.scanStepIndex - 1 : S_STEP_100_0kHz;
    }

    settings.frequencyChangeStep = GetBW() >> 1;
    RelaunchScan();
    ResetBlacklist();
    redrawScreen = true;
}

static void UpdateCurrentFreq(bool inc)
{
    if (inc && currentFreq < F_MAX)
    {
        currentFreq += settings.frequencyChangeStep;
    }
    else if (!inc && currentFreq > F_MIN)
    {
        currentFreq -= settings.frequencyChangeStep;
    }
    else
    {
        return;
    }
    RelaunchScan();
    ResetBlacklist();
    redrawScreen = true;
}

static void UpdateCurrentFreqStill(bool inc)
{
    uint8_t offset = modulationTypeTuneSteps[settings.modulationType];
    uint32_t f = fMeasure;
    if (inc && f < F_MAX)
    {
        f += offset;
    }
    else if (!inc && f > F_MIN)
    {
        f -= offset;
    }
    SetF(f);
    redrawScreen = true;
}

static void UpdateFreqChangeStep(bool inc)
{
    uint16_t diff = GetScanStep() * 4;
    if (inc && settings.frequencyChangeStep < 200000)
    {
        settings.frequencyChangeStep += diff;
    }
    else if (!inc && settings.frequencyChangeStep > 10000)
    {
        settings.frequencyChangeStep -= diff;
    }
    SYSTEM_DelayMs(100);
    redrawScreen = true;
}

static void ToggleModulation()
{
    if (settings.modulationType < MODULATION_UKNOWN - 1)
    {
        settings.modulationType++;
    }
    else
    {
        settings.modulationType = MODULATION_FM;
    }
    RADIO_SetModulation(settings.modulationType);

    RelaunchScan();
    redrawScreen = true;
}

static void ToggleListeningBW()
{
    if (settings.listenBw == BK4819_FILTER_BW_NARROWER)
    {
        settings.listenBw = BK4819_FILTER_BW_WIDE;
    }
    else
    {
        settings.listenBw++;
    }
    redrawScreen = true;
}

static void ToggleBacklight()
{
    settings.backlightState = !settings.backlightState;
    if (settings.backlightState)
    {
        BACKLIGHT_TurnOn();
    }
    else
    {
        BACKLIGHT_TurnOff();
    }
}

static void ToggleStepsCount()
{
    if (settings.stepsCount == STEPS_128)
    {
        settings.stepsCount = STEPS_16;
    }
    else
    {
        settings.stepsCount--;
    }
    settings.frequencyChangeStep = GetBW() >> 1;
    RelaunchScan();
    ResetBlacklist();
    redrawScreen = true;
}

static void ResetFreqInput()
{
    tempFreq = 0;
    for (int i = 0; i < 10; ++i)
    {
        freqInputString[i] = '-';
    }
}

static void FreqInput()
{
    freqInputIndex = 0;
    freqInputDotIndex = 0;
    ResetFreqInput();
    SetState(FREQ_INPUT);
}

static void UpdateFreqInput(KEY_Code_t key)
{
    if (key != KEY_EXIT && freqInputIndex >= 10)
    {
        return;
    }
    if (key == KEY_STAR)
    {
        if (freqInputIndex == 0 || freqInputDotIndex)
        {
            return;
        }
        freqInputDotIndex = freqInputIndex;
    }
    if (key == KEY_EXIT)
    {
        freqInputIndex--;
        if (freqInputDotIndex == freqInputIndex)
            freqInputDotIndex = 0;
    }
    else
    {
        freqInputArr[freqInputIndex++] = key;
    }

    ResetFreqInput();

    uint8_t dotIndex =
        freqInputDotIndex == 0 ? freqInputIndex : freqInputDotIndex;

    KEY_Code_t digitKey;
    for (int i = 0; i < 10; ++i)
    {
        if (i < freqInputIndex)
        {
            digitKey = freqInputArr[i];
            freqInputString[i] = digitKey <= KEY_9 ? '0' + digitKey - KEY_0 : '.';
        }
        else
        {
            freqInputString[i] = '-';
        }
    }

    uint32_t base = 100000; // 1MHz in BK units
    for (int i = dotIndex - 1; i >= 0; --i)
    {
        tempFreq += (freqInputArr[i] - KEY_0) * base;
        base *= 10;
    }

    base = 10000; // 0.1MHz in BK units
    if (dotIndex < freqInputIndex)
    {
        for (int i = dotIndex + 1; i < freqInputIndex; ++i)
        {
            tempFreq += (freqInputArr[i] - KEY_0) * base;
            base /= 10;
        }
    }
    redrawScreen = true;
}

static void Blacklist()
{
#ifdef ENABLE_SCAN_RANGES
    blacklistFreqs[blacklistFreqsIdx++ % ARRAY_SIZE(blacklistFreqs)] = peak.i;
#endif

    SetRssiHistory(peak.i, RSSI_MAX_VALUE);
    ResetPeak();
    ToggleRX(false);
    ResetScanStats();
}

#ifdef ENABLE_SCAN_RANGES
static bool IsBlacklisted(uint16_t idx)
{
    if (blacklistFreqsIdx)
        for (uint8_t i = 0; i < ARRAY_SIZE(blacklistFreqs); i++)
            if (blacklistFreqs[i] == idx)
                return true;
    return false;
}
#endif

// Draw things

// applied x2 to prevent initial rounding
uint8_t Rssi2PX(uint16_t rssi, uint8_t pxMin, uint8_t pxMax)
{
    const int DB_MIN = settings.dbMin << 1;
    const int DB_MAX = settings.dbMax << 1;
    const int DB_RANGE = DB_MAX - DB_MIN;

    const uint8_t PX_RANGE = pxMax - pxMin;

    int dbm = clamp(Rssi2DBm(rssi) << 1, DB_MIN, DB_MAX);

    return ((dbm - DB_MIN) * PX_RANGE + DB_RANGE / 2) / DB_RANGE + pxMin;
}

uint8_t Rssi2Y(uint16_t rssi)
{
    return DrawingEndY - Rssi2PX(rssi, 0, DrawingEndY);
}

// Smooth the spectrum data by averaging adjacent frequency bins
static void SmoothSpectrum(uint16_t bars)
{
    // Initialize smoothed with current values
    for (uint8_t i = 0; i < bars; ++i)
    {
        spectrum_smoothed[i] = rssiHistory[(bars>128) ? i >> settings.stepsCount : i];
    }
    
    // Apply smoothing window (simple moving average)
    uint16_t temp[128];
    for (uint8_t i = 0; i < bars; ++i)
    {
        uint32_t sum = 0;
        uint8_t count = 0;
        
        for (int8_t j = -SPECTRUM_SMOOTH_WINDOW; j <= SPECTRUM_SMOOTH_WINDOW; ++j)
        {
            int16_t idx = (int16_t)i + j;
            if (idx >= 0 && idx < bars)
            {
                uint16_t val = spectrum_smoothed[idx];
                // Only average valid signals; skip invalid and near-zero values to prevent smearing
                if (val != RSSI_MAX_VALUE && val > 0)
                {
                    sum += val;
                    count++;
                }
            }
        }
        
        if (count > 0)
        {
            temp[i] = sum / count;
        }
        else
        {
            temp[i] = RSSI_MAX_VALUE;
        }
    }
    
    // Copy back
    for (uint8_t i = 0; i < bars; ++i)
    {
        spectrum_smoothed[i] = temp[i];
    }
}

// Update peak hold values with age-based decay
static void UpdateSpectrumPeaks(uint16_t bars)
{
    for (uint8_t i = 0; i < bars; ++i)
    {
        uint16_t current = spectrum_smoothed[i];
        
        // Update peak if current value exceeds stored peak
        if (current != RSSI_MAX_VALUE && 
            (spectrum_peaks[i] == RSSI_MAX_VALUE || current > spectrum_peaks[i]))
        {
            spectrum_peaks[i] = current;
            spectrum_peak_age[i] = 0;
        }
        
        // Age the peak values (decay)
        if (spectrum_peak_age[i] < SPECTRUM_PEAK_HOLD_TIME)
        {
            spectrum_peak_age[i]++;
        }
        else
        {
            // Reset peak after hold time expires
            spectrum_peaks[i] = RSSI_MAX_VALUE;
        }
    }
}

#ifdef ENABLE_FEAT_F4HWN
    static void DrawSpectrum()
    {
        uint16_t steps = GetStepsCount();
        // max bars at 128 to correctly draw larger numbers of samples
        uint8_t bars = (steps > 128) ? 128 : steps;

        // Apply smoothing to current spectrum
        SmoothSpectrum(bars);
        
        // Update peak hold values with decay
        UpdateSpectrumPeaks(bars);

        uint8_t ox = 0;
        for (uint8_t i = 0; i < bars; ++i)
        {
            uint16_t rssi = spectrum_smoothed[i];
            uint16_t peak_rssi = spectrum_peaks[i];
            
#ifdef ENABLE_SCAN_RANGES
            uint8_t x;
            if (gScanRangeStart && bars > 1)
            {
                // Total width units = (bars - 1) full bars + 2 half bars = bars
                // First bar: half width, middle bars: full width, last bar: half width
                // Scale: 128 pixels / (bars - 1) = pixels per full bar
                uint16_t fullWidth = 128 * 2 / (bars - 1);  // x2 for precision
                
                if (i == 0)
                {
                    x = fullWidth / 4;  // half of half (because fullWidth is x2)
                }
                else
                {
                    // Position = half + (i-1) full bars + current bar
                    x = fullWidth / 4 + (uint16_t)i * fullWidth / 2;
                    if (i == bars - 1) x = 128;  // Last bar ends at screen edge
                }
            }
            else
#endif
            {
                uint8_t shift_graph = 64 / steps + 1;
                x = i * 128 / bars + shift_graph;
            }

            // Draw current value (solid bar)
            if (rssi != RSSI_MAX_VALUE)
            {
                for (uint8_t xx = ox; xx < x; xx++)
                {
                    DrawVLine(Rssi2Y(rssi), DrawingEndY, xx, true);
                }
            }
            
            // Draw peak hold indicator (thin line at peak)
            if (peak_rssi != RSSI_MAX_VALUE && peak_rssi >= rssi)
            {
                uint8_t peak_y = Rssi2Y(peak_rssi);
                for (uint8_t xx = ox; xx < x; xx++)
                {
                    // Draw every other pixel for distinct visual appearance
                    if (xx % 2 == 0)
                    {
                        PutPixel(xx, peak_y, true);
                    }
                }
            }
            
            ox = x;
        }
    }
#else
    static void DrawSpectrum()
    {
        // Non-F4HWN version: still apply smoothing for better appearance
        SmoothSpectrum(128);
        UpdateSpectrumPeaks(128);
        
        for (uint8_t x = 0; x < 128; ++x)
        {
            uint16_t rssi = spectrum_smoothed[x];
            uint16_t peak_rssi = spectrum_peaks[x];
            
            // Draw current value
            if (rssi != RSSI_MAX_VALUE)
            {
                DrawVLine(Rssi2Y(rssi), DrawingEndY, x, true);
            }
            
            // Draw peak indicator
            if (peak_rssi != RSSI_MAX_VALUE && peak_rssi >= rssi && x % 2 == 0)
            {
                uint8_t peak_y = Rssi2Y(peak_rssi);
                PutPixel(x, peak_y, true);
            }
        }
    }
#endif

// Waterfall definitions (file-scope)
// Professional 4x4 Bayer matrix (values 0..15, optimized for smooth dithering)
// This matrix provides excellent gray-level distribution for single-bit displays
static const uint8_t gBayer4x4[4][4] = {
    {0,   8,  2,  10},   // Row 0: even distribution
    {12,  4,  14, 6},    // Row 1: diagonal pattern
    {3,   11, 1,  9},    // Row 2: complementary to row 0
    {15,  7,  13, 5}     // Row 3: complementary to row 1
};

// Add a new waterfall row (scrolls older rows down). Uses current `rssiHistory`.
static void Waterfall_AddLine(void)
{
    // Shift all rows down (oldest discarded)
    for (int r = WATERFALL_ROWS_PIXELS - 1; r > 0; --r)
    {
        memcpy(waterfall_rows[r], waterfall_rows[r - 1], sizeof(waterfall_rows[0]));
    }

    // Advance temporal Bayer phase for better dithering quality
    // Cycles through 0→1→2→3→0 to provide 4-level effective gray in time domain
    waterfall_phase = (waterfall_phase + 1) & 3;
    waterfall_scan_count++;

    // Build new top row using professional Bayer dithering
    memset(waterfall_rows[0], 0, sizeof(waterfall_rows[0]));
    
    for (int x = 0; x < 128; ++x)
    {
        // Get RSSI value synchronized with spectrum graph
        // rssiHistory stores 128 frequency bins, indexed 0-127
        uint16_t rssi = rssiHistory[x];
        if (rssi == RSSI_MAX_VALUE)
            continue;  // Skip invalid measurements

        // Convert RSSI to dBm
        int dbm = Rssi2DBm(rssi);
        int dbmin = settings.dbMin;
        int dbmax = settings.dbMax;
        
        // Ensure valid range
        if (dbmax <= dbmin) 
            dbmax = dbmin + 1;
        
        // Map dBm to 0-15 level for dithering (4 bits of resolution)
        // This provides 16 effective gray levels with temporal dithering
        int lev = (dbm - dbmin) * 15 / (dbmax - dbmin + 1);
        if (lev < 0) lev = 0;
        if (lev > 15) lev = 15;

        // Get spatial position in 4x4 Bayer matrix
        int bx = x & 3;           // x position in matrix (0-3)
        int by = waterfall_phase & 3;  // temporal phase (0-3, cycles with each scan)

        // Compare level against threshold - creates dithered pixel
        // Temporal phase variation provides smooth animation effect
        if ((uint8_t)lev > gBayer4x4[by][bx])
        {
            waterfall_rows[0][x >> 3] |= (1 << (x & 7));
        }
    }
}

// Render waterfall buffer into frame buffer with professional appearance
static void DrawWaterfall(void)
{
    // Process 2 pages (16 pixels = 2 bytes per page)
    for (int p = 0; p < WATERFALL_PAGES; ++p)
    {
        // Process each column to convert bit array to framebuffer format
        for (int col = 0; col < 128; ++col)
        {
            uint8_t b = 0;
            
            // Pack 8 rows into one framebuffer byte (vertical bit layout)
            for (int bit = 0; bit < 8; ++bit)
            {
                int row = p * 8 + bit;
                if (row < WATERFALL_ROWS_PIXELS)
                {
                    // Extract bit from waterfall row
                    uint8_t byte = waterfall_rows[row][col >> 3];
                    uint8_t bitv = (byte >> (col & 7)) & 1;
                    b |= (bitv << bit);
                }
            }
            gFrameBuffer[WATERFALL_PAGE_START + p][col] = b;
        }
    }
}

static void DrawStatus()
{

#ifdef SPECTRUM_EXTRA_VALUES
    sprintf(String, "%d/%d P:%d T:%d", settings.dbMin, settings.dbMax,
            Rssi2DBm(peak.rssi), Rssi2DBm(settings.rssiTriggerLevel));
#else
    sprintf(String, "%d/%d", settings.dbMin, settings.dbMax);
#endif
    GUI_DisplaySmallest(String, 0, 1, true, true);

    BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[gBatteryCheckCounter++ % 4],
                             &gBatteryCurrent);

    uint16_t voltage = (gBatteryVoltages[0] + gBatteryVoltages[1] +
                        gBatteryVoltages[2] + gBatteryVoltages[3]) /
                       4 * 760 / gBatteryCalibration[3];

    unsigned perc = BATTERY_VoltsToPercent(voltage);

    // sprintf(String, "%d %d", voltage, perc);
    // GUI_DisplaySmallest(String, 48, 1, true, true);

    gStatusLine[116] = 0b00011100;
    gStatusLine[117] = 0b00111110;
    for (int i = 118; i <= 126; i++)
    {
        gStatusLine[i] = 0b00100010;
    }

    for (unsigned i = 127; i >= 118; i--)
    {
        if (127 - i <= (perc + 5) * 9 / 100)
        {
            gStatusLine[i] = 0b00111110;
        }
    }
}

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
static void ShowChannelName(uint32_t f)
{
    static uint32_t channelF = 0;
    static char channelName[12]; 

    if (isListening)
    {
        if (f != channelF) {
            channelF = f;
            unsigned int i;
            memset(channelName, 0, sizeof(channelName));
            for (i = 0; IS_MR_CHANNEL(i); i++)
            {
                if (RADIO_CheckValidChannel(i, false, 0))
                {
                    if (SETTINGS_FetchChannelFrequency(i) == channelF)
                    {
                        SETTINGS_FetchChannelName(channelName, i);
                        break;
                    }
                }
            }
        }
        if (channelName[0] != 0) {
            UI_PrintStringSmallBufferNormal(channelName, gStatusLine + 36);
        }
    }
    else
    {
        memset(&gStatusLine[36], 0, 100 - 28);
    }
    ST7565_BlitStatusLine();
}
#endif

static void DrawF(uint32_t f)
{
    sprintf(String, "%u.%05u", f / 100000, f % 100000);
    UI_PrintStringSmallNormal(String, 8, 127, 0);

    sprintf(String, "%3s", gModulationStr[settings.modulationType]);
    GUI_DisplaySmallest(String, 116, 1, false, true);
    sprintf(String, "%4sk", bwOptions[settings.listenBw]);
    GUI_DisplaySmallest(String, 108, 7, false, true);

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
    ShowChannelName(f);
#endif
}

static void DrawNums()
{

    if (currentState == SPECTRUM)
    {
#ifdef ENABLE_SCAN_RANGES
        if (gScanRangeStart)
        {
            sprintf(String, "%ux", GetStepsCountDisplay());
        }
        else
#endif
        {
            sprintf(String, "%ux", GetStepsCount());
        }
        GUI_DisplaySmallest(String, 0, 1, false, true);
        sprintf(String, "%u.%02uk", GetScanStep() / 100, GetScanStep() % 100);
        GUI_DisplaySmallest(String, 0, 7, false, true);
    }

    if (IsCenterMode())
    {
        sprintf(String, "%u.%05u \x7F%u.%02uk", currentFreq / 100000,
                currentFreq % 100000, settings.frequencyChangeStep / 100,
                settings.frequencyChangeStep % 100);
        GUI_DisplaySmallest(String, 36, 49, false, true);
    }
    else
    {
        sprintf(String, "%u.%05u", GetFStart() / 100000, GetFStart() % 100000);
        GUI_DisplaySmallest(String, 0, 49, false, true);

        sprintf(String, "\x7F%u.%02uk", settings.frequencyChangeStep / 100,
                settings.frequencyChangeStep % 100);
        GUI_DisplaySmallest(String, 48, 49, false, true);

        sprintf(String, "%u.%05u", GetFEnd() / 100000, GetFEnd() % 100000);
        GUI_DisplaySmallest(String, 93, 49, false, true);
    }
}

static void DrawRssiTriggerLevel()
{
    if (settings.rssiTriggerLevel == RSSI_MAX_VALUE || monitorMode)
        return;
    uint8_t y = Rssi2Y(settings.rssiTriggerLevel);
    for (uint8_t x = 0; x < 128; x += 2)
    {
        PutPixel(x, y, true);
    }
}

static void DrawTicks()
{
    uint32_t f = GetFStart();
    uint32_t span = GetFEnd() - GetFStart();
    uint32_t step = span / 128;
    for (uint8_t i = 0; i < 128; i += (1 << settings.stepsCount))
    {
        f = GetFStart() + span * i / 128;
        uint8_t barValue = 0b00000001;
        (f % 10000) < step && (barValue |= 0b00000010);
        (f % 50000) < step && (barValue |= 0b00000100);
        (f % 100000) < step && (barValue |= 0b00011000);

        gFrameBuffer[RULER_PAGE][i] |= barValue;
    }

    // center
    if (IsCenterMode())
    {
        memset(gFrameBuffer[RULER_PAGE] + 62, 0x80, 5);
        gFrameBuffer[RULER_PAGE][64] = 0xff;
    }
    else
    {
        memset(gFrameBuffer[RULER_PAGE] + 1, 0x80, 3);
        memset(gFrameBuffer[RULER_PAGE] + 124, 0x80, 3);

        gFrameBuffer[RULER_PAGE][0] = 0xff;
        gFrameBuffer[RULER_PAGE][127] = 0xff;
    }
}

static void DrawArrow(uint8_t x)
{
    for (signed i = -2; i <= 2; ++i)
    {
        signed v = x + i;
        if (!(v & 128))
        {
            gFrameBuffer[RULER_PAGE][v] |= (0b01111000 << my_abs(i)) & 0b01111000;
        }
    }
}

static void OnKeyDown(uint8_t key)
{
    switch (key)
    {
    case KEY_3:
        UpdateDBMax(true);
        break;
    case KEY_9:
        UpdateDBMax(false);
        break;
    case KEY_1:
        UpdateScanStep(true);
        break;
    case KEY_7:
        UpdateScanStep(false);
        break;
    case KEY_2:
        UpdateFreqChangeStep(true);
        break;
    case KEY_8:
        UpdateFreqChangeStep(false);
        break;
    case KEY_UP:
#ifdef ENABLE_SCAN_RANGES
        if (!gScanRangeStart)
#endif
#ifdef ENABLE_NAVIG_LEFT_RIGHT
            UpdateCurrentFreq(false);
#else
            UpdateCurrentFreq(true);
#endif
        break;
    case KEY_DOWN:
#ifdef ENABLE_SCAN_RANGES
        if (!gScanRangeStart)
#endif
#ifdef ENABLE_NAVIG_LEFT_RIGHT
            UpdateCurrentFreq(true);
#else
            UpdateCurrentFreq(false);
#endif
        break;
    case KEY_SIDE1:
        Blacklist();
        break;
    case KEY_STAR:
        UpdateRssiTriggerLevel(true);
        break;
    case KEY_F:
        UpdateRssiTriggerLevel(false);
        break;
    case KEY_5:
#ifdef ENABLE_SCAN_RANGES
        if (!gScanRangeStart)
#endif
            FreqInput();
        break;
    case KEY_0:
        ToggleModulation();
        break;
    case KEY_6:
        ToggleListeningBW();
        break;
    case KEY_4:
#ifdef ENABLE_SCAN_RANGES
        if (!gScanRangeStart)
#endif
            ToggleStepsCount();
        break;
    case KEY_SIDE2:
        ToggleBacklight();
        break;
    case KEY_PTT:
        SetState(STILL);
        TuneToPeak();
        break;
    case KEY_MENU:
        break;
    case KEY_EXIT:
        if (menuState)
        {
            menuState = 0;
            break;
        }
#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
        SaveSettings();
#endif
#ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
        gEeprom.CURRENT_STATE = 0;
        SETTINGS_WriteCurrentState();
#endif
        DeInitSpectrum();
        break;
    default:
        break;
    }
}

static void OnKeyDownFreqInput(uint8_t key)
{
    switch (key)
    {
    case KEY_0:
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_8:
    case KEY_9:
    case KEY_STAR:
        UpdateFreqInput(key);
        break;
    case KEY_EXIT:
        if (freqInputIndex == 0)
        {
            SetState(previousState);
            break;
        }
        UpdateFreqInput(key);
        break;
    case KEY_MENU:
        if (tempFreq < F_MIN || tempFreq > F_MAX)
        {
            break;
        }
        SetState(previousState);
        currentFreq = tempFreq;
        if (currentState == SPECTRUM)
        {
            ResetBlacklist();
            RelaunchScan();
        }
        else
        {
            SetF(currentFreq);
        }
        break;
    default:
        break;
    }
}

void OnKeyDownStill(KEY_Code_t key)
{
    switch (key)
    {
    case KEY_3:
        UpdateDBMax(true);
        break;
    case KEY_9:
        UpdateDBMax(false);
        break;
    case KEY_UP:
        if (menuState)

#ifdef ENABLE_NAVIG_LEFT_RIGHT
        {
            SetRegMenuValue(menuState, false);
            break;
        }
        UpdateCurrentFreqStill(false);
#else
        {
            SetRegMenuValue(menuState, true);
            break;
        }
        UpdateCurrentFreqStill(true);
#endif
        break;
    case KEY_DOWN:
        if (menuState)
#ifdef ENABLE_NAVIG_LEFT_RIGHT
        {
            SetRegMenuValue(menuState, true);
            break;
        }
        UpdateCurrentFreqStill(true);
#else
        {
            SetRegMenuValue(menuState, false);
            break;
        }
        UpdateCurrentFreqStill(false);
#endif
        break;
    case KEY_STAR:
        UpdateRssiTriggerLevel(true);
        break;
    case KEY_F:
        UpdateRssiTriggerLevel(false);
        break;
    case KEY_5:
        FreqInput();
        break;
    case KEY_0:
        ToggleModulation();
        break;
    case KEY_6:
        ToggleListeningBW();
        break;
    case KEY_SIDE1:
        monitorMode = !monitorMode;
        break;
    case KEY_SIDE2:
        ToggleBacklight();
        break;
    case KEY_PTT:
        // TODO: start transmit
        /* BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
        BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true); */
        break;
    case KEY_MENU:
        if (menuState == ARRAY_SIZE(registerSpecs) - 1)
        {
            menuState = 1;
        }
        else
        {
            menuState++;
        }
        redrawScreen = true;
        break;
    case KEY_EXIT:
        if (!menuState)
        {
            SetState(SPECTRUM);
            lockAGC = false;
            monitorMode = false;
            RelaunchScan();
            break;
        }
        menuState = 0;
        break;
    default:
        break;
    }
}

static void RenderFreqInput() { UI_PrintString(freqInputString, 2, 127, 0, 8); }

static void RenderStatus()
{
    memset(gStatusLine, 0, sizeof(gStatusLine));
    DrawStatus();
    ST7565_BlitStatusLine();
}

static void RenderSpectrum()
{
    DrawTicks();
    DrawArrow(128u * peak.i / GetStepsCount());
    DrawSpectrum();
    DrawWaterfall();
    DrawRssiTriggerLevel();
    DrawF(peak.f);
    DrawNums();
}

static void RenderStill()
{
    DrawF(fMeasure);

    const uint8_t METER_PAD_LEFT = 3;

    memset(&gFrameBuffer[2][METER_PAD_LEFT], 0b00010000, 121);

    for (int i = 0; i < 121; i += 5)
    {
        gFrameBuffer[2][i + METER_PAD_LEFT] = 0b00110000;
    }

    for (int i = 0; i < 121; i += 10)
    {
        gFrameBuffer[2][i + METER_PAD_LEFT] = 0b01110000;
    }

    uint8_t x = Rssi2PX(scanInfo.rssi, 0, 121);
    for (int i = 0; i < x; ++i)
    {
        if (i % 5)
        {
            gFrameBuffer[2][i + METER_PAD_LEFT] |= 0b00000111;
        }
    }

    int dbm = Rssi2DBm(scanInfo.rssi);
    uint8_t s = DBm2S(dbm);
    sprintf(String, "S: %u", s);
    GUI_DisplaySmallest(String, 4, 25, false, true);
    sprintf(String, "%d dBm", dbm);
    GUI_DisplaySmallest(String, 28, 25, false, true);

    if (!monitorMode)
    {
        uint8_t x = Rssi2PX(settings.rssiTriggerLevel, 0, 121);
        gFrameBuffer[2][METER_PAD_LEFT + x] = 0b11111111;
    }

    const uint8_t PAD_LEFT = 4;
    const uint8_t CELL_WIDTH = 30;
    uint8_t offset = PAD_LEFT;
    uint8_t row = 4;

    for (int i = 0, idx = 1; idx <= 4; ++i, ++idx)
    {
        if (idx == 5)
        {
            row += 2;
            i = 0;
        }
        offset = PAD_LEFT + i * CELL_WIDTH;
        if (menuState == idx)
        {
            for (int j = 0; j < CELL_WIDTH; ++j)
            {
                gFrameBuffer[row][j + offset] = 0xFF;
                gFrameBuffer[row + 1][j + offset] = 0xFF;
            }
        }
        sprintf(String, "%s", registerSpecs[idx].name);
        GUI_DisplaySmallest(String, offset + 2, row * 8 + 2, false,
                            menuState != idx);

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
        if(idx == 1)
        {
            sprintf(String, "%ddB", LNAsOptions[GetRegMenuValue(idx)]);
        }
        else if(idx == 2)
        {
            sprintf(String, "%ddB", LNAOptions[GetRegMenuValue(idx)]);
        }
        else if(idx == 3)
        {
            sprintf(String, "%ddB", VGAOptions[GetRegMenuValue(idx)]);
        }
        else if(idx == 4)
        {
            sprintf(String, "%skHz", BPFOptions[(GetRegMenuValue(idx) / 0x2aaa)]);
        }
#else
        sprintf(String, "%u", GetRegMenuValue(idx));
#endif
        GUI_DisplaySmallest(String, offset + 2, (row + 1) * 8 + 1, false,
                            menuState != idx);
    }
}

static void Render()
{
    UI_DisplayClear();

    switch (currentState)
    {
    case SPECTRUM:
        RenderSpectrum();
        break;
    case FREQ_INPUT:
        RenderFreqInput();
        break;
    case STILL:
        RenderStill();
        break;
    }

    ST7565_BlitFullScreen();
}

static bool HandleUserInput()
{
    kbd.prev = kbd.current;
    kbd.current = GetKey();

    if (kbd.current != KEY_INVALID && kbd.current == kbd.prev)
    {
        if (kbd.counter < 16)
            kbd.counter++;
        else
            kbd.counter -= 3;
        SYSTEM_DelayMs(20);
    }
    else
    {
        kbd.counter = 0;
    }

    if (kbd.counter == 3 || kbd.counter == 16)
    {
        switch (currentState)
        {
        case SPECTRUM:
            OnKeyDown(kbd.current);
            break;
        case FREQ_INPUT:
            OnKeyDownFreqInput(kbd.current);
            break;
        case STILL:
            OnKeyDownStill(kbd.current);
            break;
        }
    }

    return true;
}

static void Scan()
{
    if (rssiHistory[scanInfo.i] != RSSI_MAX_VALUE
#ifdef ENABLE_SCAN_RANGES
        && !IsBlacklisted(scanInfo.i)
#endif
    )
    {
        SetF(scanInfo.f);
        Measure();
        UpdateScanInfo();
    }
}

static void NextScanStep()
{
    ++peak.t;
    ++scanInfo.i;
    scanInfo.f += scanInfo.scanStep;
}

static void UpdateScan()
{
    Scan();

    if (scanInfo.i < scanInfo.measurementsCount)
    {
        NextScanStep();
        return;
    }

    // Always clear unused bins to prevent stale data from previous scans
    memset(&rssiHistory[scanInfo.measurementsCount], 0,
           sizeof(rssiHistory) - scanInfo.measurementsCount * sizeof(rssiHistory[0]));

    redrawScreen = true;
    preventKeypress = false;
    
    // Clear peak hold immediately when scan completes (don't wait for next scan)
    // This prevents ghost peaks from lingering after signal disappears
    memset(spectrum_peaks, 0xFF, sizeof(spectrum_peaks));
    memset(spectrum_peak_age, 0, sizeof(spectrum_peak_age));

    UpdatePeakInfo();
    if (IsPeakOverLevel())
    {
        ToggleRX(true);
        TuneToPeak();
        return;
    }

    // push a new waterfall line for this completed scan
    Waterfall_AddLine();

    newScanStart = true;
}

static void UpdateStill()
{
    Measure();
    redrawScreen = true;
    preventKeypress = false;

    peak.rssi = scanInfo.rssi;
    AutoTriggerLevel();

    if (IsPeakOverLevel() || monitorMode) {
        ToggleRX(true);
    }
}

static void UpdateListening()
{
    preventKeypress = false;
    #ifdef ENABLE_FEAT_F4HWN_SPECTRUM
    bool tailFound = checkIfTailFound();
    if (tailFound)
    #else
    if (currentState == STILL)
    #endif
    {
        listenT = 0;
    }
    if (listenT)
    {
        listenT--;
        SYSTEM_DelayMs(1);
        return;
    }

    if (currentState == SPECTRUM)
    {
        BK4819_WriteRegister(0x43, GetBWRegValueForScan());
        Measure();
        BK4819_WriteRegister(0x43, listenBWRegValues[settings.listenBw]);
    }
    else
    {
        Measure();
    }

    peak.rssi = scanInfo.rssi;
    redrawScreen = true;

    #ifdef ENABLE_FEAT_F4HWN_SPECTRUM
        if ((IsPeakOverLevel() && !tailFound) || monitorMode)
        {
            listenT = 100;
            return;
        }
    #else
        if (IsPeakOverLevel() || monitorMode)
        {
            listenT = 1000;
            return;
        }
    #endif

    ToggleRX(false);
    ResetScanStats();
}

static void Tick()
{
#ifdef ENABLE_AM_FIX
    if (gNextTimeslice)
    {
        gNextTimeslice = false;
        if (settings.modulationType == MODULATION_AM && !lockAGC)
        {
            AM_fix_10ms(vfo); // allow AM_Fix to apply its AGC action
        }
    }
#endif

#ifdef ENABLE_SCAN_RANGES
    if (gNextTimeslice_500ms)
    {
        gNextTimeslice_500ms = false;

        // if a lot of steps then it takes long time
        // we don't want to wait for whole scan
        // listening has it's own timer
        if (GetStepsCount() > 128 && !isListening)
        {
            UpdatePeakInfo();
            if (IsPeakOverLevel())
            {
                ToggleRX(true);
                TuneToPeak();
                return;
            }
            redrawScreen = true;
            preventKeypress = false;
        }
    }
#endif

    if (!preventKeypress)
    {
        HandleUserInput();
    }
    if (newScanStart)
    {
        InitScan();
        newScanStart = false;
    }
    if (isListening && currentState != FREQ_INPUT)
    {
        UpdateListening();
    }
    else
    {
        if (currentState == SPECTRUM)
        {
            UpdateScan();
        }
        else if (currentState == STILL)
        {
            UpdateStill();
        }
    }
    if (redrawStatus || ++statuslineUpdateTimer > 4096)
    {
        RenderStatus();
        redrawStatus = false;
        statuslineUpdateTimer = 0;
    }
    if (redrawScreen)
    {
        Render();
        // For screenshot
        #ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
            getScreenShot(false);
        #endif
        redrawScreen = false;
    }
}

void APP_RunSpectrum()
{
    // TX here coz it always? set to active VFO
    vfo = gEeprom.TX_VFO;
#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
    LoadSettings();
#endif
    // set the current frequency in the middle of the display
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        currentFreq = initialFreq = gScanRangeStart;
        for (uint8_t i = 0; i < ARRAY_SIZE(scanStepValues); i++)
        {
            if (scanStepValues[i] >= gTxVfo->StepFrequency)
            {
                settings.scanStepIndex = i;
                break;
            }
        }
        settings.stepsCount = STEPS_128;
        #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
            gEeprom.CURRENT_STATE = 5;
        #endif
    }
    else {
#endif
        currentFreq = initialFreq = gTxVfo->pRX->Frequency -
                                    ((GetStepsCount() / 2) * GetScanStep());
        #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
            gEeprom.CURRENT_STATE = 4;
        #endif
    }

    #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
        SETTINGS_WriteCurrentState();
    #endif

    #ifdef ENABLE_FEAT_F4HWN_SPECTRUM
        LoadSettings();
    #endif
    BackupRegisters();

    isListening = true; // to turn off RX later
    redrawStatus = true;
    redrawScreen = true;
    newScanStart = true;

    ToggleRX(true);
    ToggleRX(false); // hack to prevent noise when squelch off
    settings.modulationType = gTxVfo->Modulation;
    RADIO_SetModulation(settings.modulationType);

#ifdef ENABLE_FEAT_F4HWN_SPECTRUM
    BK4819_SetFilterBandwidth(settings.listenBw, false);
#else
    settings.listenBw = BK4819_FILTER_BW_WIDE;
    BK4819_SetFilterBandwidth(settings.listenBw, false);
#endif

    RelaunchScan();

    memset(rssiHistory, 0, sizeof(rssiHistory));
    memset(waterfall_rows, 0, sizeof(waterfall_rows));
    waterfall_phase = 0;
    waterfall_scan_count = 0;

    isInitialized = true;

    while (isInitialized)
    {
        Tick();
    }
}