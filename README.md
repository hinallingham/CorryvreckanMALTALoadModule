# EventLoaderMALTA

**Maintainer**: Hinata Nakamura (hnakamura@quark.hiroshima-u.ac.jp)
**Module Type**: DETECTOR
**Status**: Functional / Active Development

## Description
The `EventLoaderMALTA` module is a dedicated data loader designed to interface MALTA2 raw data (stored in ROOT TTree format) with the Corryvreckan framework. Unlike standard loaders, this module implements a robust synchronization logic specifically for the MALTA2 DAQ output, utilizing the **L1ID (Level-1 ID)** as the primary trigger reference.

The module follows a **Master-Slave synchronization architecture**:
1. **Master Plane (Plane 0)**: Defines the target L1ID for the current Corryvreckan event.
2. **Slave Planes**: The loader scans the data stream for matching L1IDs to group hits from the same physical particle incident into a single event clipboard.

This ensures high-fidelity event building even in high-occupancy environments or scenarios with significant data-stream latencies between planes.

## Key Features
* **Smart Path Builder**: Automatically resolves file paths using a base directory and a 6-digit run number (e.g., `run_000123_0.root.root`).
* **L1ID Synchronization**: Trigger-ID based event building that handles plane-to-plane clock drifts.
* **Sync Health Monitor**: Real-time tracking of the coincidence rate with automatic console warnings (LOG(WARNING)) if synchronization stability drops.
* **Integrated Masking**: Full support for Corryvreckan's standard `.txt` mask files via the `detector->isMasked(x, y)` interface.
* **Beam Profile Analytics**: Automatic generation of 2D hit maps and 1D projections for immediate beam spot and divergence evaluation.

---

## Parameters

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `base_path` | string | Directory path containing the raw ROOT files. |
| `run_number` | int | The run ID used to automatically find and load files. |
| `detector_names`| array | List of detector names defined in the geometry file (order matters). |

## Plots Produced

### 2D Histograms
* **`hHitMap_<detector>`**: Global hit occupancy map for beam profile visualization.
* **`hCoincidenceRateTrend`**: A `TProfile` showing the coincidence rate [%] vs. Event Number to monitor synchronization stability throughout the run.

### 1D Histograms
* **`hHitX / hHitY`**: Projections of the hit map for beam width ($\sigma$) and centroid calculations.
* **`hSyncRate`**: Distribution of the global coincidence rate.

---

## Usage
```ini
[EventLoaderMALTA]
base_path = "/home/user/data/beamtest_2026"
run_number = 112
detector_names = "Plane0", "Plane1", "Plane2"