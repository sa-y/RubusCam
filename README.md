# RubusCam

Raspberry Pi 上で **SDL2**、**SDL2_image**、**SDL2_ttf**、および **libcamera** を利用し、カメラ映像のリアルタイムプレビュー表示と高解像度静止画撮影を行うデジタルスチルカメラ (DSC) ツールです。  
`libcamera` は C++20 API のみを提供しているため、軽量な `extern "C"` ラッパー層を介して **C11** のコア実装から安全かつシンプルに制御しています。

---

## 主な特徴

- **低遅延 GPU プレビュー & タッチ/マウス GUI**: `SDL_Renderer` による約30 FPS のアスペクト比維持描画と、`SDL2_ttf` による半透明 OSD・タッチ操作ボタンを実装。X11/Wayland 環境だけでなくコンソール (`SDL_VIDEODRIVER=kmsdrm`) 全画面実行にも対応。
- **高解像度静止画キャプチャ & 非同期保存パイプライン**: プレビュー用ストリーム (`Viewfinder`) とは別にセンサー最大解像度 (`StillCapture`: 例 `4608x2592`) で撮影。シャッター直後にクイックレビューを即座に表示しつつ、オンメモリ JPEG エンコード・Exif 2.3 メタデータ埋め込み・SDカード書き込みを専用ワーカースレッドで非同期処理。
- **本格的なカメラ制御 (AF / 露出 / WB / 測光)**: 常時追従AF・ワンショットAF・画面タップによる**タッチAF**に加え、露出補正 (`±3.0 EV`)、ISO感度 (`Auto / 100〜3200`)、シャッター速度 (`Auto / 1/2000s〜2.0s`)、ホワイトバランス（プリセット / 色温度 `2500K〜10000K` / AWBロック）、測光モード（評価 / 中央重点 / スポット）、JPEG画質（3段階）の調整に対応。
- **再生・ギャラリー & DCF メディア管理**: クイックレビュー（即時破棄・保護対応）および再生モードを備え、Exif 詳細表示、デジタル拡大・パン（`2x / 4x / 8x` + PiP ナビマップ）、`3x3 / 4x4` サムネイル一覧、プロテクト保護・削除に対応。ファイルは DCF 準拠（`DCIM/100_PICA/IMG_0001.JPG` または `--date-dir` による日付別）で自動採番。
- **C11 コア + libcamera C++20 ラッパー設計**: カメラ制御を不透明構造体 (`CameraStream`) と C 関数群にカプセル化し、`Ctrl + C` 割り込み時も安全にリソースを解放。

---

## 動作要件

- **ハードウェア**: Raspberry Pi (Raspberry Pi 4 / 5 等) + Raspberry Pi Camera Module (Module 3 / v2 等)
- **OS**: Raspberry Pi OS (Debian 12 Bookworm / Debian 11 Bullseye 推奨)
- **コンパイラ**: GCC 10 以上 または Clang 10 以上（C11 および C++20 対応）

### 依存パッケージのインストール

```bash
sudo apt update
sudo apt install -y libcamera-dev libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev fonts-dejavu-core build-essential cmake pkg-config
```

---

## ビルド方法

### 方法 1: CMake を使用する場合 (推奨)

```bash
cmake -B build
cmake --build build
```

### 方法 2: Makefile を使用する場合

```bash
make
```

いずれの方法でも、ビルド完了後に `build/rubuscam` が生成されます。

---

## クイックスタート

```bash
# 通常起動 (DCIM/100_PICA/IMG_0001.JPG 形式で保存)
./build/rubuscam

# 日付別フォルダモード (DCIM/YYYYMMDD/IMG_0001.JPG 形式) で起動
./build/rubuscam --date-dir
```

### 基本操作（抜粋）

| 操作 | キーボード | タッチパネル / マウス |
| :--- | :--- | :--- |
| **静止画撮影** | `Space` | 右中央の **`[ SHOT ]`** ボタンをタップ |
| **オートフォーカス** | `F` | **`[ AF [F] ]`** ボタン、またはプレビュー画面の任意の位置をタップ（タッチAF） |
| **撮影設定の変更** | `I` (ISO), `S` (SS), `W` (WB), `Up`/`Down` (EV) 等 | 左上の **`[ Settings ]`** を開いて各項目をタップ / ホイール回転 |
| **再生・ギャラリー** | `P` | 右下の **`[ Play [P] ]`** ボタンをタップ |
| **設定リセット** | `R` | 左上の **`[ Reset [R] ]`** ボタンをタップ |
| **終了** | `Q` または `Ctrl + C` | 右上の **`[ Quit [Q] ]`** ボタンをタップ |

> 全モードのキーボードショートカット一覧、タッチ・マウス操作の詳細、解像度のカスタマイズ方法については **[ユーザーガイド (docs/USER_GUIDE.md)](docs/USER_GUIDE.md)** を参照してください。

---

## ドキュメント一覧

- **[ユーザーガイド (`docs/USER_GUIDE.md`)](docs/USER_GUIDE.md)**: 起動オプション、全モードのキーボード操作表、タッチ・マウス GUI 操作方法、DCF 保存仕様、カスタマイズ方法
- **[アーキテクチャ & 内部設計 (`docs/ARCHITECTURE.md`)](docs/ARCHITECTURE.md)**: ディレクトリ構成、各モジュールの役割、非同期保存スレッドモデル、`camera_stream.h` C API リファレンス
- **[機能実装ロードマップ (`docs/TODO.md`)](docs/TODO.md)**: デジタルスチルカメラ (DSC) としての実装状況および今後の拡張計画

---

## ライセンス

[MIT License](LICENSE)
