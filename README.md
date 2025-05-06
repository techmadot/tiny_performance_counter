# tiny_performance_counter

## これは何？

1ヘッダファイルで完結するパフォーマンスカウンタライブラリです。
CPUやGPUの使用率を取得するものです。 Windows11 のタスクマネージャーで確認できるものと同等です。


## 使い方

### インクルードと実装

単一ヘッダライブラリとして作成しているため、tiny_performance_counter.h ファイルをインクルードして使用します。
実装コードを有効化するために、C/C++ソースファイルを選択、もしくは用意します。
そのソースファイルにおいて、tiny_performance_counter.h ファイルをインクルードする前に、`TINY_PERFORMANCE_COUNTER_IMPLEMENTATION` を定義してください。
これにより実装コードが有効となり、コンパイルされます。

### サンプル

サンプルコードは、 sample_main.cpp として用意しています。
各プロセッサの使用率は以下のように取得できます。

```cpp
#define TINY_PERFORMANCE_COUNTER_IMPLEMENTATION // 実装をコンパイルさせるために定義.
#include "tiny_performance_counter.h"

// 初期化
tiny_perf_counter::InitParams initParams{};
initParams.useGlobalCPUUtilization = true;  // タスクマネージャーでの CPU 使用率と合う形式で採取する.
tiny_perf_counter::Initialize(initParams);


// CPU 使用率
auto cpuUsage = tiny_perf_counter::GetCPUUtilization();

// GPU 使用率
auto gpuUsage = tiny_perf_counter::GetGPUEngineUtilization();

// 終了処理.
tiny_perf_counter::Shutdown();
```

## 特徴

- CPU/GPU の使用率を取得
- CPU の各コアごとの使用率を取得
- GPU の各エンジンの使用率を取得
- VRAM の使用量 (Dedicated/Shared) の取得

### 動作プラットフォーム

- Windows11 x64

## その他

サポートなどはありませんが、不具合など発見の際には教えて頂ければと思います。
また、プルリクエストも歓迎いたします。
