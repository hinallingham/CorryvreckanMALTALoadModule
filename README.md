# EventLoaderMALTA

## Overview / 概要

**English:**
This module is a custom `EventLoader` for Corryvreckan, specifically designed to read MALTA2 detector data directly from ROOT files. It was developed to bypass the complexities and potential errors associated with converting ROOT data to EUDAQ2 RAW formats. It serves as a **Global Module**, managing the synchronization of multiple MALTA2 planes within a single process.

**日本語:**
このモジュールは、MALTA2検出器のデータをROOTファイルから直接Corryvreckanに読み込むためのカスタム`EventLoader`です。ROOTからEUDAQ2 RAW形式への変換に伴うエラーや手間を省くために開発されました。**グローバルモジュール**として動作し、1つのプロセスで複数のMALTA2レイヤーの同期を管理します。

---

## Key Features / 主な特徴

- **Direct ROOT Access**: No need for intermediate file conversion.
- **L1ID-Based Synchronization**: Automatically synchronizes "Lower" and "Upper" sensor data by matching their `L1ID` (Level 1 Trigger ID).
- **Sweep Logic**: Robustly handles data by sweeping all hits associated with a specific `L1ID` into a single Corryvreckan `Event`.
- **Modern C++ Compliance**: Optimized to avoid compiler warnings and ensure memory safety within the Corryvreckan framework.

- **ROOT直読み**: 中間ファイル（.raw）への変換が不要。
- **L1IDベースの同期**: `L1ID`（トリガーID）をキーにして、LowerとUpperのセンサーデータを自動的に同期。
- **スイープ・ロジック**: 特定の`L1ID`に紐付く全ヒットを1つのCorryvreckan `Event`として確実に回収する堅牢な実装。
- **モダンC++準拠**: フレームワーク内でのメモリ安全性を確保し、コンパイラ警告を排除した最適化済みコード。

---

## Parameters / パラメータ

| Parameter | Description |
| :--- | :--- |
| `file_lower` | Path to the ROOT file for the Lower detector (MALTA_0). |
| `file_upper` | Path to the ROOT file for the Upper detector (MALTA_1). |

| パラメータ | 説明 |
| :--- | :--- |
| `file_lower` | Lower検出器（MALTA_0）のROOTファイルパス。 |
| `file_upper` | Upper検出器（MALTA_1）のROOTファイルパス。 |

---

## Usage / 使い方

Add the following block to your Corryvreckan configuration (`.conf`) file:
Corryvreckanのコンフィグファイル（`.conf`）に以下のブロックを追加して使用します。

```ini
[EventLoaderMALTA]
file_lower = "/path/to/your/lower_data.root"
file_upper = "/path/to/your/upper_data.root"