# RubusCam 

Raspberry Pi 上で **SDL2**、**SDL2_image**、**SDL2_ttf**、および **libcamera** を利用し、カメラ映像のリアルタイムプレビュー表示と静止画撮影を行うカメラビューアツールです。  
`libcamera` は公式に C++ API のみを提供しているため、C言語から安全かつシンプルに呼び出せるよう、`extern "C"` で囲んだ軽量な C++ ラッパー層を用意して連携しています。

---

## 主な特徴

- **C言語によるコア実装**: メインループ、SDL2 GPU レンダリング処理、SDL2_ttf テキスト描画、シグナル制御を C11 で実装。
- **libcamera C++ ラッパー (`extern "C"`)**: libcamera のストリーム制御・フレーム取得・AF制御・JPEGキャプチャを不透明構造体 (`CameraStream`) と C インターフェース関数群でカプセル化。
- **GPU ハードウェアアクセラレーション描画**: `SDL_Renderer` とストリーミングテクスチャ (`SDL_Texture`) により、低遅延（約30 FPS）でカメラ映像を描画。
- **アスペクト比維持スケーリング**: ウィンドウサイズや画面解像度に合わせて、映像の比率を崩さず自動でレターボックス／ピラーボックス表示。
- **オートフォーカス対応**: Camera Module 3 等のオートフォーカス対応カメラにおいて、プレビュー中も自動でピントを合わせ続ける「常時追従AF (Continuous AF)」が有効。
- **ワンキー撮影**: `Space` キーを押すだけで、高品質な静止画 (JPEG) を即座にファイル保存。
- **手動リフォーカス**: `F` キーを押すことで、任意のタイミングでワンショットAF再調整が可能。
- **美麗なGUIオーバーレイ表示**: `SDL2_ttf` によるアンチエイリアス TrueType フォントと半透明背景ボックスを GPU 合成。
- **コンソール (CLI) / KMSDRM 環境対応**: X11/Wayland などのデスクトップ環境がないコンソール環境でも、`SDL_VIDEODRIVER=kmsdrm` によりマウスカーソル非表示で軽快に全画面描画。
- **高解像度静止画ストリームの分離**: プレビュー表示（`Viewfinder`: 例 `1024x600`）とは別に、センサー本来の最大解像度（`StillCapture`: 例 `4608x2592`）での静止画キャプチャに対応（2系統マルチストリーム構成およびモード切替フォールバック対応）。
- **DCF 準拠のファイル命名・ディレクトリ管理**: カメラ業界標準の `DCIM/100_PICA/IMG_0001.JPG` 形式（または `--date-dir` 指定で日付別 `DCIM/YYYYMMDD/IMG_0001.JPG`）で自動連番保存し、撮影前にストレージ残容量を事前確認。
- **シグナルハンドリング**: `Ctrl + C` などの割り込み時も、カメラストリームと SDL リソースを安全に解放して終了。

---

## 動作要件

- **ハードウェア**:
  - Raspberry Pi (Raspberry Pi 4 / 5 等)
  - Raspberry Pi Camera Module (Camera Module 3 / v2 等)
- **OS**: Raspberry Pi OS (Debian 12 Bookworm / Debian 11 Bullseye 推奨)
- **開発ツール**:
  - C コンパイラ (GCC 10 以上 / Clang 10 以上、C11 対応)
  - C++ コンパイラ (GCC 10 以上 / Clang 10 以上、C++20 対応)

---

## 依存パッケージのインストール

Raspberry Pi 上で以下のコマンドを実行し、必要な開発用ライブラリを導入します。

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

ビルド完了後、`build/rubscam` が生成されます。

### 方法 2: Makefile を使用する場合 (シンプルビルド)

```bash
make
```

`build/RubusCam` に実行ファイルが生成されます。

---

## 使い方

以下のコマンドでアプリケーションを起動します。

```bash
# デフォルト起動 (DCIM/100_PICA/IMG_0001.JPG 形式)
./build/rubscam

# 日付別フォルダモード (DCIM/YYYYMMDD/IMG_0001.JPG 形式) で起動する場合
./build/rubscam --date-dir
```

### キーボード操作

| キー | 動作 |
| :--- | :--- |
| **`Space`** | 高解像度静止画を撮影して保存します (`DCIM/100_PICA/IMG_0001.JPG` 等) |
| **`F`** | オートフォーカスを再調整します (ワンショットAF / AF対応カメラのみ) |
| **`Up` / `Down`** または **`]` / `[`** | 露出補正 (`-3.0 EV` 〜 `+3.0 EV`、`1/3 EV` ステップ) |
| **`I`** / **`Shift + I`** | ISO感度を切り替え (`Auto / 100 / 200 / 400 / 800 / 1600 / 3200`) |
| **`S`** / **`Shift + S`** または **`Right` / `Left`** | シャッタースピードを切り替え (`Auto / 1/2000s 〜 2.0s`) |
| **`R`** | 露出設定をすべてリセット (`EV: 0.0`, `ISO: Auto`, `SS: Auto`) |
| **`Q`** | アプリケーションを終了します |
| **`Ctrl + C`** | コンソールからの割り込み終了（リソースを安全に解放して終了します） |

---

## ディレクトリ構成

```text
picamera/
├── CMakeLists.txt       # CMake ビルド設定 (C & C++ 混在ビルド)
├── Makefile             # 簡易 make ビルド設定 (gcc + g++)
├── LICENSE              # MIT ライセンス
├── README.md            # 本ドキュメント
├── TODO.md              # 機能拡張ロードマップ
├── fonts/               # オーバーレイ表示用バンドル TrueType フォント (IPAex)
├── include/
│   ├── camera_stream.h  # libcamera ラッパー関数の C 向け公開ヘッダー (extern "C")
│   ├── dcf_storage.h    # DCF 準拠ディレクトリ・連番ファイル管理・残容量確認ヘッダー
│   ├── font8x16.h       # 8x16 ビットマップフォント定義ヘッダー
│   ├── text_renderer.h  # SDL2_ttf 用テキスト・ガイド描画関数宣言
│   └── stb_image_write.h# JPEG 保存用ヘッダーライブラリ
└── src/
    ├── main.c           # アプリケーションエントリポイント & SDL2 イベント/レンダリングループ (C言語)
    ├── camera_stream.cpp# libcamera 制御実装 & extern "C" ラッパー関数群 (C++)
    ├── dcf_storage.c    # DCF フォルダ自動生成・連番採番・statvfs 残容量チェック実装 (C言語)
    ├── font8x16.c       # 8x16 ビットマップフォントデータ (C言語)
    └── text_renderer.c  # SDL2_ttf ガイドオーバーレイ描画実装 (C言語)
```

---

## libcamera ラッパー関数 API 一覧 (`camera_stream.h`)

C言語側からは以下の `extern "C"` 関数を通じてカメラを操作できます。

```c
CameraStream *camera_stream_create(void);
void camera_stream_destroy(CameraStream *cam);

void camera_stream_set_still_size(CameraStream *cam, int still_width, int still_height);
bool camera_stream_init(CameraStream *cam, int width, int height);
bool camera_stream_start(CameraStream *cam);
void camera_stream_stop(CameraStream *cam);

bool camera_stream_is_af_supported(const CameraStream *cam);
int camera_stream_get_width(const CameraStream *cam);
int camera_stream_get_height(const CameraStream *cam);
int camera_stream_get_stride(const CameraStream *cam);
size_t camera_stream_get_frame_size(const CameraStream *cam);
bool camera_stream_is_bgr(const CameraStream *cam);

int camera_stream_get_still_width(const CameraStream *cam);
int camera_stream_get_still_height(const CameraStream *cam);

bool camera_stream_get_latest_frame(CameraStream *cam, uint8_t *out_buffer, size_t buffer_size, uint64_t *out_sequence);
bool camera_stream_trigger_af(CameraStream *cam);

typedef bool (*CameraStillSaveCallback)(const char *filename, const uint8_t *data, int width, int height, int stride, bool is_bgr);
bool camera_stream_capture_still(CameraStream *cam, const char *filename, CameraStillSaveCallback save_cb);
```

---

## カスタマイズ

解像度を変更したい場合は、[`src/main.c`](src/main.c) 内の以下の値を変更して再ビルドしてください。

```c
const int screen_width = 1024;
const int screen_height = 600;
```

フレームレートを調整する場合は、メインループ内の `target_frame_time_ms`（デフォルト: `1000 / 30`）を変更します。

---

## 今後の機能拡張ロードマップ

デジタルスチルカメラ (DSC) としての機能拡張一覧および実装計画については、[TODO.md](TODO.md) を参照してください。

---

## ライセンス

MIT License
