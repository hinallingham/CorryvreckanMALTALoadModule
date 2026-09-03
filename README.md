<div align="right">
  🌐 &nbsp;<strong>English</strong> &nbsp;|&nbsp; <a href="README_ja.md">日本語</a>
</div>

<h1 align="center">EventLoaderMALTA</h1>
<p align="center">
  MALTA2 raw data loader for the Corryvreckan reconstruction framework
</p>
<p align="center">
  <img src="https://img.shields.io/badge/maintainer-Hinata%20Nakamura-informational?style=flat-square" />
  <img src="https://img.shields.io/badge/type-DETECTOR-blueviolet?style=flat-square" />
  <img src="https://img.shields.io/badge/status-Active%20Development-brightgreen?style=flat-square" />
  <img src="https://img.shields.io/badge/near--online-supported-orange?style=flat-square" />
</p>

---

## Description

`EventLoaderMALTA` loads MALTA2 hit data stored as ROOT TTrees by MaltaMultiDAQ
into the Corryvreckan clipboard. It uses the **L1ID** as the primary trigger reference
to synchronize hits across multiple detector planes.

**Synchronization model**

1. **Plane 0 (Master)** — sets the target L1ID for each Corryvreckan event.
2. **Remaining planes (Slaves)** — matched against the same L1ID to form a coincident event.
3. A candidate L1ID only becomes a real Corryvreckan event if every non-DUT ("reference") plane
   has a hit at it; DUT planes are exempt. See **Coincidence requirement** below.

**`Pixel` field semantics**

MALTA hits carry no charge-equivalent (`ToT`/`ADC`) value. `charge` is therefore always set to `1`, and the `raw()`
field — which would normally hold that charge-equivalent value — is repurposed to carry the original `bcid` of the hit
instead. `winid`/`phase` are not stored separately, but since `timestamp = bcid * 25ns + winid * 3.125ns +
phase * 0.39ns`, they can be recovered from the residual `timestamp - bcid * 25ns` (see `ClusterDump` for an example).
Be aware that any downstream module treating `raw()` as an actual charge value (e.g. `Clustering4D` with
`charge_weighting = true`) will misinterpret this as charge; keep `charge_weighting = false` for MALTA detectors.

---

## Key Features

| Feature | Description |
| :--- | :--- |
| **l1idC event grouping** | Uses 32-bit non-wrapping `l1idC` counter instead of 12-bit `l1id` — avoids rollover after 4096 triggers on real hardware |
| L1ID synchronization | Trigger-ID based event building; handles inter-plane clock drifts |
| Multi-run stitching | Chain multiple ROOT files seamlessly via `run_number` array |
| Explicit file list | Override auto path with `file_names` for custom setups (bypasses `.root.root` / `.root` extension difference) |
| Column filter | Process only `even` / `odd` columns (useful for cross-check studies) |
| Sync health monitor | Coincidence rate tracked per 1000 events with automatic `LOG(WARNING)` |
| Integrated masking | Corryvreckan `.txt` mask files via `detector->masked(x, y)` |
| **Near-online mode** | TChain is recreated from scratch on each refresh cycle — avoids ROOT's stale internal `fEntries` problem that prevented `GetEntry()` from reading newly written rows |
| **Dynamic-axis trends** | All TProfile histograms start with a small range and auto-extend + rebin to ~200 bins regardless of total event count |
| **Coincidence requirement** | Single-plane noise/dark-count L1IDs (and ROI-violated L1IDs) never become an Event — every non-DUT plane must agree. See **Coincidence requirement** below |

---

## Parameters

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `detector_names` | array\<string\> | — | Plane names in geometry file order |
| `base_path` | string | — | Directory containing raw ROOT files |
| `run_number` | array\<int\> | — | Run ID(s) to load (supports multi-run merge) |
| `file_names` | array\<string\> | — | Explicit file paths (overrides `base_path` / `run_number`) |
| `skip_entries` | int | `0` | Skip the first N entries |
| `number_of_events` | int | `0` | Stop after N events (0 = unlimited) |
| `column_filter` | string | `"none"` | `"none"` / `"even"` / `"odd"` |
| `online_mode` | bool | `false` | Keep the run alive when data is exhausted (near-online streaming) |
| `real_data` | bool | `false` | Apply `y -= 288` offset for real MALTA2 hardware. Active pixels on real chips occupy rows 288–511 in raw coordinates; this shifts them to 0–223 so they fall within the detector bounds. Leave unset (or `false`) for simulation where rows run 0–511 from the start. |

---

## Plots Produced

All TProfile trend histograms use a dynamic x-axis: initial range is `[skip_entries, skip_entries+200]`
with `SetCanExtend`, and bins are merged back to ~200 each time ROOT doubles the range on fill.

| Name | Type | Description |
| :--- | :--- | :--- |
| `hCoincidenceRateTrend` | TProfile | Coincidence rate [%] vs. event number (DUT participation rate since the coincidence requirement, see below) |
| `hL1idAcceptanceRateTrend` | TProfile | l1id acceptance rate [%] vs. event number — 100/candidates scanned to find each accepted event; low values mean many single-plane noise/ROI-violated l1ids were discarded first. See **Coincidence Requirement** below |
| `hHitsPerEventTrend_<det>` | TProfile | Mean hits/event over time per plane |
| `hHitsPerEventDist_<det>` | TH1F | Distribution of hits/event per plane |
| `hL1idCVsEvent` | TProfile | l1idC (hardware trigger count) vs. Corryvreckan event — slope > 1 means missed triggers |
| `hL1idCPlaneDelta` | TH1F | Distribution of max(l1idC)−min(l1idC) across planes per event — 0 = perfect sync |
| `hTimerVsEvent` | TProfile | Wall-clock elapsed time [s] vs. event number (MaltaDAQ only; 0 for simulation) |

---

## Event Tags

| Tag | Description |
| :--- | :--- |
| `wall_clock_time` | Elapsed run time in seconds (same source as `hTimerVsEvent`), attached to every `Event` via `addTag()`. Downstream modules (e.g. `TopupSearch`) can read it with `clipboard->getEvent()->getTag("wall_clock_time")` to build plots against real run time instead of event number or the (locally-scoped) cluster timestamp. |

---

## Usage

```ini
[EventLoaderMALTA]
base_path      = "/data/beamtest_2026"
run_number     = 112, 113
detector_names = "Plane0", "Plane1", "Plane2"
column_filter  = "none"
```

For a near-online session (MaltaMultiDAQ running simultaneously):

```ini
[EventLoaderMALTA]
base_path      = "/data/beamtest_2026"
run_number     = 115
detector_names = "Plane0", "Plane1", "Plane2"
online_mode    = true    # keep alive when data is temporarily exhausted
# number_of_events not set → runs until stopped manually
```

---

## Near-Online Monitoring

### How it works

When `online_mode = true`, the loader does not stop when it runs out of entries.
Instead, every 200 ms it **recreates the TChain** from the stored file paths so that ROOT
reads a fully fresh file header.  This is more reliable than `TChain::Refresh()`, which
updates the entry count externally but leaves the internal `TTree::fEntries` stale —
causing `GetEntry()` to silently skip new rows.

```
MaltaDAQ write loop          EventLoaderMALTA run() loop
──────────────────           ────────────────────────────
FillTree(hits)               GetEntry(i)  → process
...                          ...
AutoSave()  ──flush──►       (200 ms)  new TChain created
FillTree(hits)               GetEntry(new_i)  → process new entries
```

When `online_mode = false` (default), the loader returns `StatusCode::EndRun` as soon as
all entries are consumed, which is the correct behaviour for offline reprocessing.

### Requirements for near-online mode

| Requirement | Detail |
| :--- | :--- |
| `online_mode = true` in config | Without this the loader exits on the first `NoData` condition |
| MaltaDAQ must call `AutoSave()` periodically | `Refresh()` only picks up entries that have been flushed to disk; unflushed entries are invisible. Recommended cadence: every ~100 events (~1 s) |
| Single writer, single reader on the same machine | ROOT does not support concurrent writers. Reading while one process writes is safe for local filesystems. NFS may cache stale file headers and cause missed entries |
| `number_of_events` should be `0` or unset | Setting a finite limit causes `EndRun` once that count is reached, regardless of `online_mode` |

### Files Changed

| File | Line(s) | What was added |
| :--- | :--- | :--- |
| `EventLoaderMALTA.h` | 4 | `#include <chrono>` |
| `EventLoaderMALTA.h` | 66 | `std::chrono::steady_clock::time_point last_refresh_time_` member |
| `EventLoaderMALTA.cpp` | 89–93 | `LOG(WARNING)` on 0 entries instead of `InvalidValueError` |
| `EventLoaderMALTA.cpp` | 102 | `last_refresh_time_` initialization in `initialize()` |
| `EventLoaderMALTA.cpp` | 199–223 | Full refresh block in `run()`: `Refresh()` + `GetEntries()` + `setBranchAddresses()` + `NoData` |

---

### Problems Fixed

| # | Location | Root cause | Fix |
| :--- | :--- | :--- | :--- |
| 1 | **Writer** (`Malta2TreeWriter`) | AutoSave called only when hits exist — silent stall when beam rate is low | Moved AutoSave check before the early-return path |
| 2 | **Reader** `run()` — refresh | `TChain::Refresh()` updated external entry count but left internal `TTree::fEntries` stale; `GetEntry()` beyond old end silently returned nothing | Replaced with full TChain recreation every 200 ms |
| 3 | **Reader** `run()` — l1id | Real hardware l1id is a 12-bit counter (wraps at 4096); after rollover planes appear out of sync | Switched event grouping to `l1idC` (32-bit non-wrapping) in both reader and writer |

### Implementation

**Fix 1 — AutoSave before early return** (`Malta2TreeWriter::run()`):

```cpp
// AutoSave checked first — runs even when the event has no hits
auto now = std::chrono::steady_clock::now();
if (elapsed_ms >= 1000.0) {
    tree->AutoSave("SaveSelf");
    last_autosave_time_ = now;
}
try {
    hit_messages = messenger_->fetchMultiMessage<PixelHitMessage>(this, event);
} catch (const MessageNotFoundException&) {
    return;   // early return, but AutoSave already ran above
}
```

**Fix 2 — TChain recreation instead of Refresh()**, throttled to once per 200 ms:

```cpp
if (!any_data_left && ms_elapsed >= 200.0) {
    for (size_t i = 0; i < n_planes; ++i) {
        auto fresh = std::make_unique<TChain>("MALTA");
        for (const auto& path : chain_file_paths_[i]) fresh->Add(path.c_str());
        Long64_t new_entries = fresh->GetEntries();
        if (new_entries > num_entries_[i]) {
            chains_[i]->ResetBranchAddresses();
            chains_[i] = std::move(fresh);
            data_buffer_[i].setBranchAddresses(chains_[i].get());
            num_entries_[i] = new_entries;
        }
    }
}
```

**Fix 3 — l1idC event grouping** (both writer and reader):

```cpp
// Writer (Malta2TreeWriter): fill l1idC with the event number
l1idC_ = static_cast<unsigned int>(event->number);

// Reader (EventLoaderMALTA): group by l1idC, not l1id
uint32_t current = data_buffer_[i].l1idC;   // was: data_buffer_[i].l1id
```

### Stopping a near-online session

With `number_of_events = 0` (unlimited), the run ends only when corryvreckan is stopped
manually — via Ctrl-C or the **Exit** button in `OnlineMonitor`.

---

## Coincidence Requirement

### The problem

MaltaMultiDAQ writes hits zero-suppressed: a plane only gets a tree entry for an L1ID it actually
saw a pixel fire on. Earlier, `run()` treated the minimum L1ID among *any* plane's next unread
entry as "the event", regardless of whether other planes had anything at that L1ID. In practice
this meant an isolated noise/dark-count hit on a single plane — one that shares its L1ID with no
other plane — became its own Corryvreckan Event, with every other plane contributing zero hits.
Symptoms:

- Corryvreckan's own event count (and `number_of_events`) counted these noise-only L1IDs the same
  as real multi-plane hits, so it no longer meant "N real events".
- Per-plane cluster-count trend plots (e.g. `OnlineMonitor`'s Timeline canvas) were dominated by
  degenerate 0/1 flapping, since most "events" had zero hits on any given plane.
- The same applied to L1IDs where a hit fell outside ROI: all hits were cleared but the (now
  empty) L1ID was still emitted as an Event.

### The fix

`run()` now only accepts an L1ID as a real event once every **non-DUT** plane
(`!hasRole(DUT)`/`AUXILIARY`/`PASSIVE`, computed once in `initialize()` as `coincidence_required_`)
has a hit at it, and no hit falls outside ROI. Any L1ID that fails either check is discarded —
not emitted as an empty/partial Event — and the scan silently continues to the next L1ID within
the same `run()` call. **DUT planes are exempt from this requirement on purpose**: requiring the
DUT to match too would systematically discard genuine DUT-inefficiency events, which is exactly
the thing an efficiency measurement needs to see. With this fix, `hCoincidenceRateTrend` /
`coincidence_ev_count` (previously "did every plane, DUT included, have a hit") is now closer to
a live DUT-participation-rate proxy, since reference-plane coincidence is guaranteed by
construction.

This is always on (no config flag) — it is treated as a bug fix, not an opt-in behaviour change.
If your `detector_names` list contains no non-DUT planes at all, the requirement has nothing to
check against and every L1ID is accepted, same as before.

### Consequence for `number_of_events`

Because a single `run()` call can now silently consume many discarded L1IDs before returning,
`number_of_events` (and Corryvreckan's own progress counter) means "N real coincident events",
not "N L1IDs attempted" — the intended, correct semantics, but worth knowing if you were used to
the old, inflated count.

### Trade-off

Isolated single-plane hits (e.g. for measuring a bare plane's raw noise rate) are no longer
visible to Corryvreckan at all once discarded — they never reach `Clustering4D`, `Tracking4D`, or
any downstream module. If per-plane noise-rate studies independent of coincidence are needed
later, that would require a separate diagnostic path (not currently implemented).

To at least keep the *aggregate* discard rate visible, `hL1idAcceptanceRateTrend` (and the
`total_l1id_scanned_`/`total_l1id_accepted_` counters behind the periodic `LOG(INFO)` health
check) track what fraction of scanned l1id candidates were discarded, without exposing which
individual hits were thrown away. A falling acceptance rate is an early warning that noise (or
ROI violations) are eating an increasing share of the data stream, even though the discarded
candidates themselves stay invisible.

---

## See Also

- [Malta2 README](../../../../MALTA2/Malta2/README.md) — writer-side changes (`AutoSave`)
- [OnlineMonitor](../OnlineMonitor/) — real-time display module
