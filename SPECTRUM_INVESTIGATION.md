# Deep Investigation: Spectrum Graph Not Falling to Floor After Strong Signal

## Problem Statement
After a strong signal appears on the spectrum display, the graph doesn't completely fall back to the noise floor baseline. Instead, it appears to "stick" at an elevated level or fall very slowly.

## Root Cause Analysis

### Issue #1: Peak Hold Visual Persistence ⚠️ CRITICAL
**Location**: [DrawSpectrum()](App/app/spectrum.c#L976-L1070)
**Severity**: HIGH

**Problem**: 
The new peak hold feature draws dotted indicators at peak values. However, these peaks decay over 20 frames (SPECTRUM_PEAK_HOLD_TIME). If a signal is strong, the peak indicator will stay visible even after the actual signal bar falls to zero.

**Evidence**:
```c
// Lines 1039-1047
// Draw peak hold indicator (thin line at peak)
if (peak_rssi != RSSI_MAX_VALUE && peak_rssi >= rssi)
{
    uint8_t peak_y = Rssi2Y(peak_rssi);
    for (uint8_t xx = ox; xx < x; xx++)
    {
        if (xx % 2 == 0)
        {
            PutPixel(xx, peak_y, true);  // ← Still visible while decaying!
        }
    }
}
```

**Why it happens**:
- Peak value (e.g., 200) stored in `spectrum_peaks[]`
- Signal disappears → current RSSI becomes 0
- Peak hold shows dotted line at level 200
- Peak decays over 20 frames
- User sees "floating peak line" instead of signal falling to floor

### Issue #2: Smoothing Carry-Over Effect ⚠️ CRITICAL
**Location**: [SmoothSpectrum()](App/app/spectrum.c#L905-L948)
**Severity**: HIGH

**Problem**:
The smoothing function uses a 3-bin averaging window. When a strong signal ends:
- Bin with strong signal: say value 200
- Adjacent bins: 0
- Average = (200 + 0 + 0) / 3 = 66
- This creates a "shadow" around the original signal location

**Evidence**:
```c
// Lines 918-932: Averaging logic
for (int8_t j = -SPECTRUM_SMOOTH_WINDOW; j <= SPECTRUM_SMOOTH_WINDOW; ++j)
{
    int16_t idx = (int16_t)i + j;
    if (idx >= 0 && idx < bars)
    {
        uint16_t val = spectrum_smoothed[idx];
        if (val != RSSI_MAX_VALUE)
        {
            sum += val;
            count++;
        }
    }
}

if (count > 0)
{
    temp[i] = sum / count;  // ← Averaging old values!
}
```

**Why it happens**:
- Smoothing reads from old `rssiHistory` data
- After signal disappears, those old values are still in the history
- Averaging spreads out the residual values
- Creates a "smear" effect around where the signal was

### Issue #3: RSSI History Not Properly Cleared Between Scans ⚠️ MEDIUM
**Location**: Multiple locations
**Severity**: MEDIUM

**Problem**:
The `rssiHistory[128]` buffer should be cleared after each scan, but there's incomplete logic:

```c
// Line 1784-1785: Only clears portion of history!
if (! (scanInfo.measurementsCount >> 7)) // if (scanInfo.measurementsCount < 128)
    memset(&rssiHistory[scanInfo.measurementsCount], 0,
           sizeof(rssiHistory) - scanInfo.measurementsCount * sizeof(rssiHistory[0]));
```

**Why it's a problem**:
- Condition only clears if `measurementsCount < 128`
- When scanning 128 or more points, nothing is cleared!
- Old values persist from previous scans
- The graph appears to "stick" at old signal levels

```c
// Line 1993: Initialization clears history, but...
memset(rssiHistory, 0, sizeof(rssiHistory));
// ... this is only called when spectrum analyzer starts
// NOT called when each new scan begins!
```

### Issue #4: ResetBlacklist() Converts RSSI_MAX_VALUE Incorrectly ⚠️ MEDIUM
**Location**: [ResetBlacklist()](App/app/spectrum.c#L520-L531)
**Severity**: MEDIUM

**Problem**:
```c
static void ResetBlacklist()
{
    for (int i = 0; i < 128; ++i)
    {
        if (rssiHistory[i] == RSSI_MAX_VALUE)
            rssiHistory[i] = 0;  // ← Converts invalid to valid (zero)?
    }
```

**Why it's wrong**:
- When a frequency is set to RSSI_MAX_VALUE (blacklisted/invalid), it should stay that way
- Converting to 0 makes it appear as "no signal" (which is correct visually but semantically wrong)
- More critically: This function is called AFTER scan completion in some paths
- It should be part of scan initialization, not cleanup

### Issue #5: Peak Hold Resets But Never Clears ⚠️ MEDIUM  
**Location**: [ResetScanStats()](App/app/spectrum.c#L501-L509)
**Severity**: MEDIUM

**Problem**:
```c
static void ResetScanStats()
{
    scanInfo.rssi = 0;
    scanInfo.rssiMax = 0;
    scanInfo.iPeak = 0;
    scanInfo.fPeak = 0;
    
    // Reset spectrum enhancements (peak hold and smoothing)
    memset(spectrum_peaks, 0xFF, sizeof(spectrum_peaks));  // 0xFF = RSSI_MAX_VALUE
    memset(spectrum_peak_age, 0, sizeof(spectrum_peak_age));
}
```

**Why it's a problem**:
- This function is called at START of scan (`InitScan()`)
- But scan completion happens at END of `UpdateScan()`
- Between scan end and next scan start, old peaks persist and decay
- Creates visual "ghost" peaks for one complete scan cycle

---

## Issues Summary Table

| # | Issue | Cause | Effect | Severity |
|---|-------|-------|--------|----------|
| 1 | Peak hold visual persistence | Dotted peak lines decay slowly | "Floating" peak indicators | HIGH |
| 2 | Smoothing carry-over | Averaging spreads old values | Signal "smear" around old location | HIGH |
| 3 | History not cleared fully | Only clears if <128 measurements | Stale values persist | MEDIUM |
| 4 | Blacklist reset timing | Called at wrong time | Semantic confusion | MEDIUM |
| 5 | Peak buffer reset timing | Only at scan start | Ghost peaks visible | MEDIUM |

---

## Technical Flow Diagram

### Current (Broken) Flow:
```
Scan Completes (UpdateScan)
    ↓
[Peak hold & old RSSI values still visible]
    ↓
Waterfall_AddLine() called
    ↓
redrawScreen = true
    ↓
RenderSpectrum() called
    ↓
DrawSpectrum():
    SmoothSpectrum() [averages old values!]
    UpdateSpectrumPeaks() [still showing peaks!]
    Display with smoothing & peaks still present
    ↓
Graph doesn't fall to floor (peaks visible, smoothing smears, history stale)
    ↓
Next scan starts (newScanStart = true)
    ↓
InitScan() called
    ↓
ResetScanStats() clears peaks FINALLY
    ↓
New scan measurement begins
```

### Expected (Clean) Flow:
```
Scan Completes
    ↓
[Clear old data immediately]
    ↓
Waterfall_AddLine()
    ↓
ResetScanStats() [clear peaks NOW]
    ↓
Clear rssiHistory[] completely
    ↓
RenderSpectrum()
    ↓
Graph shows only current, real data (falls to floor)
    ↓
Next scan starts with clean state
```

---

## Recommendations for Fixes

### Fix 1: Clear rssiHistory Unconditionally
**Priority**: HIGH
**Location**: [UpdateScan()](App/app/spectrum.c#L1773)

Change:
```c
if (! (scanInfo.measurementsCount >> 7)) // if (scanInfo.measurementsCount < 128)
    memset(&rssiHistory[scanInfo.measurementsCount], 0,
           sizeof(rssiHistory) - scanInfo.measurementsCount * sizeof(rssiHistory[0]));
```

To:
```c
// Always clear unused bins after measurement count
memset(&rssiHistory[scanInfo.measurementsCount], 0,
       sizeof(rssiHistory) - scanInfo.measurementsCount * sizeof(rssiHistory[0]));
```

### Fix 2: Move Peak Reset to Scan Completion
**Priority**: HIGH
**Location**: [UpdateScan()](App/app/spectrum.c#L1773)

After scan completion, before display, add:
```c
// Clear peak hold when scan completes (not on next scan start)
memset(spectrum_peaks, 0xFF, sizeof(spectrum_peaks));
memset(spectrum_peak_age, 0, sizeof(spectrum_peak_age));
```

### Fix 3: Optional - Disable Peak Hold Persistence
**Priority**: MEDIUM
**Location**: [SPECTRUM_PEAK_HOLD_TIME](App/app/spectrum.c#L95)

Change from:
```c
#define SPECTRUM_PEAK_HOLD_TIME 20
```

To:
```c
#define SPECTRUM_PEAK_HOLD_TIME 3  // Much shorter decay (faster fall-to-floor)
```

Or set to 1 to show peak only for 1 frame.

### Fix 4: Improve Smoothing Decay
**Priority**: MEDIUM
**Location**: [SmoothSpectrum()](App/app/spectrum.c#L905)

Add decay to smoothing to prevent carry-over:
```c
// Only smooth valid (non-zero, non-max) values
if (val != RSSI_MAX_VALUE && val > 0)
{
    sum += val;
    count++;
}
```

---

## Verification Steps

After implementing fixes, test:

1. **Visual Test**: Display strong signal → remove signal → observe baseline
   - Expected: Graph falls immediately to noise floor
   - Bad: Graph lingers or smears

2. **Peak Decay Test**: Observe peak indicators
   - Expected: Peaks visible briefly (1-3 frames) then vanish
   - Bad: Peaks visible for 20 frames

3. **Noise Floor Test**: With no signal, observe baseline
   - Expected: Flat line at bottom, clean
   - Bad: Fuzzy/smeared baseline, peaks floating

4. **Transition Test**: Strong → weak → strong signal
   - Expected: Clean transitions, no artifacts
   - Bad: "Ghosting" of previous signal level
