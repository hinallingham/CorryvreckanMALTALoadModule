<div align="right">
  🌐 &nbsp;<a href="README.md">English</a> &nbsp;|&nbsp; <strong>日本語</strong>
</div>

<h1 align="center">EventLoaderMALTA</h1>
<p align="center">
  Corryvreckan フレームワーク向け MALTA2 生データローダー
</p>
<p align="center">
  <img src="https://img.shields.io/badge/担当-Hinata%20Nakamura-informational?style=flat-square" />
  <img src="https://img.shields.io/badge/タイプ-DETECTOR-blueviolet?style=flat-square" />
  <img src="https://img.shields.io/badge/ステータス-開発中-brightgreen?style=flat-square" />
  <img src="https://img.shields.io/badge/ニアオンライン-対応済み-orange?style=flat-square" />
</p>

---

## 説明

`EventLoaderMALTA` は、MaltaMultiDAQ が ROOT TTree 形式で保存した MALTA2 ヒットデータを
Corryvreckan のクリップボードに読み込むモジュールです。
複数の検出器プレーン間のヒット同期に **L1ID** をトリガーリファレンスとして使用します。

**同期モデル**

1. **プレーン 0（マスター）** — 各 Corryvreckan イベントのターゲット L1ID を決定します。
2. **その他のプレーン（スレーブ）** — 同一 L1ID に一致するヒットを収集し、coincident イベントを構成します。

---

## 主な機能

| 機能 | 説明 |
| :--- | :--- |
| **l1idC イベントグループ化** | 12bit の `l1id`（実機で 4096 回折返し）ではなく 32bit の `l1idC` でイベントを結合 — 実機でのロールオーバー問題を解消 |
| L1ID 同期 | トリガー ID ベースのイベント構築；プレーン間クロックドリフトにも対応 |
| マルチランマージ | `run_number` 配列で複数の ROOT ファイルをシームレスに連結 |
| ファイル直接指定 | `file_names` で任意のファイルパスを指定可能（拡張子の違いを回避） |
| カラムフィルタ | 偶数 / 奇数カラムのみを処理（クロスチェック用途向け） |
| 同期健全性モニター | 1000 イベントごとに coincidence rate を集計し、低下時に `LOG(WARNING)` |
| マスキング統合 | Corryvreckan 標準 `.txt` マスクファイルを `detector->masked(x, y)` で適用 |
| **ニアオンラインモード** | TChain を毎回再作成してファイルヘッダを完全にリロード — `Refresh()` では更新されない内部 `fEntries` の問題を回避 |
| **動的横軸トレンド** | 全 TProfile は小さい初期レンジから開始し、`SetCanExtend` + rebin で常に約 200 bins を維持 |

---

## パラメータ

| パラメータ | 型 | デフォルト | 説明 |
| :--- | :--- | :--- | :--- |
| `detector_names` | array\<string\> | — | ジオメトリファイルの順序に合わせたプレーン名 |
| `base_path` | string | — | 生 ROOT ファイルが置かれているディレクトリ |
| `run_number` | array\<int\> | — | 読み込むラン番号（複数指定でマルチランマージ） |
| `file_names` | array\<string\> | — | ファイルパスの直接指定（`base_path`/`run_number` を上書き） |
| `skip_entries` | int | `0` | 先頭 N エントリをスキップ |
| `number_of_events` | int | `0` | N イベントで停止（0 = 無制限） |
| `column_filter` | string | `"none"` | `"none"` / `"even"` / `"odd"` |
| `online_mode` | bool | `false` | データが尽きてもランを継続する（ニアオンラインストリーミング用） |
| `real_data` | bool | `false` | 実機 MALTA2 ハードウェア用の `y -= 288` オフセットを適用する。実チップの活性ピクセルは生座標で 288〜511 行目に存在するため、ジオメトリの 0〜223 範囲に収めるにはこのシフトが必要。シミュレーション（行 0〜511 を使用）では未設定のまま（`false`）にすること。 |

---

## 生成されるプロット

全 TProfile トレンドは動的横軸：初期レンジ `[skip_entries, skip_entries+200]`、`SetCanExtend` で
ROOT が自動拡張するたびに rebin して約 200 bins を維持します。

| 名前 | タイプ | 内容 |
| :--- | :--- | :--- |
| `hCoincidenceRateTrend` | TProfile | coincidence rate [%] のイベント数依存性 |
| `hHitsPerEventTrend_<検出器名>` | TProfile | プレーンごとの平均ヒット数の時間変化 |
| `hHitsPerEventDist_<検出器名>` | TH1F | プレーンごとのイベントあたりヒット数分布 |
| `hL1idCVsEvent` | TProfile | l1idC（HW トリガー累積数）vs Corryvreckan イベント番号 — 傾き > 1 はトリガー取りこぼしを示す |
| `hL1idCPlaneDelta` | TH1F | プレーン間 l1idC のズレ分布 — 0 に集中 = プレーン同期 OK |
| `hTimerVsEvent` | TProfile | 実経過時間 [秒] vs イベント番号（MaltaDAQ 実機のみ有効、シミュレーションでは 0） |

---

## 使用方法

```ini
[EventLoaderMALTA]
base_path      = "/data/beamtest_2026"
run_number     = 112, 113
detector_names = "Plane0", "Plane1", "Plane2"
column_filter  = "none"
```

MaltaMultiDAQ と同時実行するニアオンラインセッションの場合：

```ini
[EventLoaderMALTA]
base_path      = "/data/beamtest_2026"
run_number     = 115
detector_names = "Plane0", "Plane1", "Plane2"
online_mode    = true    # データが一時的に尽きてもランを継続
# number_of_events を設定しない → 手動停止まで継続
```

---

## ニアオンライン監視

### 動作の仕組み

`online_mode = true` のとき、ローダーはエントリが尽きても停止しません。
代わりに `TChain::Refresh()` を（200 ms ごとにスロットリングして）呼び出し、
MaltaDAQ がその間にフラッシュした新しいエントリをファイルヘッダから再取得します。
新しいエントリが見つからなかった場合は `StatusCode::NoData` を返し、
フレームワークが直ちに `run()` を再呼び出しします。

```
MaltaDAQ 書き込みループ        EventLoaderMALTA run() ループ
──────────────────             ────────────────────────────
FillTree(hits)                 GetEntry(i)  → 処理
FillTree(hits)                 GetEntry(i)  → 処理
...                            ...
AutoSave()  ──flush──►         Refresh()    → num_entries 更新
FillTree(hits)                 GetEntry(i)  → 新エントリを処理
```

`online_mode = false`（デフォルト）の場合、全エントリを処理した時点で
`StatusCode::EndRun` を返します。オフライン再解析にはこちらが適切です。

### ニアオンラインモードの使用条件

| 条件 | 詳細 |
| :--- | :--- |
| 設定ファイルに `online_mode = true` を記載 | 未設定の場合、データが尽きた時点でランが終了する |
| MaltaDAQ 側で `AutoSave()` を定期呼び出し | `Refresh()` はディスクにフラッシュ済みのエントリしか認識できない。未フラッシュ分は見えない。推奨頻度：約 100 イベントごと（約 1 秒） |
| 同一マシン上で 1 プロセスが書き込み・1 プロセスが読み込み | ROOT は複数ライターに対応していない。ローカルファイルシステム上での単一ライター＋単一リーダーは安全。NFS はファイルヘッダをキャッシュするためエントリの読み損ねが起きる場合がある |
| `number_of_events` は `0` か未設定にする | 有限値を設定するとその件数に達した時点で `EndRun` になる（`online_mode` に関係なく） |

### 変更ファイル一覧

| ファイル | 行 | 追加内容 |
| :--- | :--- | :--- |
| `EventLoaderMALTA.h` | 4 | `#include <chrono>` |
| `EventLoaderMALTA.h` | 66 | メンバ変数 `std::chrono::steady_clock::time_point last_refresh_time_` |
| `EventLoaderMALTA.cpp` | 89–93 | エントリ数 0 時に `InvalidValueError` を投げる代わりに `LOG(WARNING)` |
| `EventLoaderMALTA.cpp` | 102 | `initialize()` 内で `last_refresh_time_` を初期化 |
| `EventLoaderMALTA.cpp` | 199–223 | `run()` にリフレッシュブロックを追加：`Refresh()` + `GetEntries()` + `setBranchAddresses()` + `NoData` 返却 |

---

### 修正されたバグ

| # | 場所 | 根本原因 | 対処 |
| :--- | :--- | :--- | :--- |
| 1 | **書き手** `Malta2TreeWriter` | AutoSave がヒットなしイベントで early return してしまい呼ばれない — ビームレートが低い時に長時間フラッシュされない | AutoSave チェックを early return より前に移動 |
| 2 | **読み手** `run()` — リフレッシュ | `TChain::Refresh()` は外部エントリ数を更新するが内部 `TTree::fEntries` は古いまま → `GetEntry()` が新エントリを無音でスキップ | 200 ms ごとに TChain を完全に再作成 |
| 3 | **読み手** `run()` — l1id | 実機の l1id は 12bit（4096 で折返し） — ロールオーバー後にプレーン間の対応が崩れる | イベントグループ化を `l1idC`（32bit 累積）に変更、書き手側も対応 |

### 実装

**修正 2 — 起動時のエントリ数ゼロを許容する**（`initialize()`）：

```cpp
num_entries_[i] = chain->GetEntries();
if (num_entries_[i] == 0) {
    LOG(WARNING) << "Plane " << i << " has 0 entries at startup."
                 << " Waiting for MaltaDAQ to write data...";
} else {
    LOG(INFO) << "Plane " << i << " Total Chained Entries = " << num_entries_[i];
}
```

**修正 3 — `run()` での動的リフレッシュ**（200 ms スロットリング）：

```cpp
if (!any_data_left) {
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - last_refresh_time_).count();
    if (ms >= 200.0) {
        for (size_t i = 0; i < n_planes; ++i) {
            chains_[i]->Refresh();
            num_entries_[i] = chains_[i]->GetEntries();
            data_buffer_[i].setBranchAddresses(chains_[i].get()); // Refresh 後に再設定
        }
        last_refresh_time_ = now;
        // 新しい最小 L1ID を再スキャン ...
    }
    if (!any_data_left)
        return StatusCode::NoData;   // ランを継続；フレームワークがすぐ再呼び出し
}
```

> **`Refresh()` 後に `setBranchAddresses()` を再呼び出す理由**  
> `TChain::Refresh()` は内部でファイルを再オープンし、すべてのブランチアドレス設定を
> リセットします。再設定しないと、以降の `GetEntry()` 呼び出しが古い（または null の）
> ポインタに書き込み、サイレントにゴミデータを生成します。

**なぜ 200 ms か？**  
MaltaMultiDAQ は `AutoSave()` を約 1 秒ごとに呼び出します。
200 ms のポーリング間隔はその 5 倍の頻度であり、
エントリを取りこぼさずに I/O も最小限に抑えられます。

### ニアオンラインセッションの終了

`number_of_events = 0`（無制限）の場合、ランは手動でのみ終了します。
corryvreckan を Ctrl-C か `OnlineMonitor` の **Exit ボタン** で停止してください。

---

## 関連リンク

- [Malta2 README（日本語）](../../../../MALTA2/Malta2/README_ja.md) — 書き手側の変更（`AutoSave`）
- [OnlineMonitor](../OnlineMonitor/) — リアルタイム表示モジュール
