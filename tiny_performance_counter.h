#ifndef TINY_PERFORMANCE_COUNTER_H
#define TINY_PERFORMANCE_COUNTER_H

/**
      MIT License

      Copyright (c) 2025 techmadot.

      Permission is hereby granted, free of charge, to any person obtaining a copy
      of this software and associated documentation files (the "Software"), to deal
      in the Software without restriction, including without limitation the rights
      to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
      copies of the Software, and to permit persons to whom the Software is
      furnished to do so, subject to the following conditions:

      The above copyright notice and this permission notice shall be included in all
      copies or substantial portions of the Software.

      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
      IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
      FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
      AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
      LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
      SOFTWARE.
 */
#include <cstdint>
#include <vector>
#include <string>

namespace tiny_perf_counter
{
  struct InitParams
  {
    bool useGlobalCPUUtilization = true;
  };
  // 初期化処理.
  bool Initialize(const InitParams& initParams);

  // 終了処理.
  void Shutdown();

  uint64_t GetUsedGPUDedicatedMemory();
  uint64_t GetUsedGPUSharedMemory();

  // GPU の使用率を取得 (3D).
  double GetGPUEngineUtilization();

  // GPU の使用率を取得 (エンジンごとの情報取得が可能).
  //  例: 3D, Copy, VideoEncode, VideoDecode, ... など.
  double GetGPUEngineUtilization(const wchar_t* engineName);

  // 使用可能な GPU エンジンの名前リストを取得.
  std::vector<std::wstring> GetGPUEngineNames();

  // CPU の使用率を取得.
  //  - useGlobalCPUUtilization = true の場合、システム全体での使用率.
  //  - useGlobalCPUUtilization = false の場合、プロセス単体での使用率.
  double GetCPUUtilization();


  // CPU コアごとの使用率を取得.
  //  - システム全体での使用率である点に注意.
  std::vector<double> GetCPUCoresUtilization();
}

#if defined(TINY_PERFORMANCE_COUNTER_IMPLEMENTATION)
#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif 
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <unordered_map>
#include <sstream>
#include <memory>
#include <thread>
#include <mutex>
#include <format>
#include <chrono>

#pragma comment(lib, "pdh.lib")

namespace tiny_perf_counter
{
  namespace impl
  {
    using namespace std::chrono_literals;

    class SimplePerfCounter
    {
    public:
      SimplePerfCounter() = default;
      ~SimplePerfCounter()
      {
        if (m_workerThread.joinable())
        {
          // スレッド側の終了処理をトリガー.
          std::unique_lock lock(m_mutex);
          m_exit = true;
          lock.unlock();

          m_condVar.notify_all();
          m_workerThread.join();
        }
        if (m_pdhQuery)
        {
          PdhCloseQuery(m_pdhQuery);
          m_pdhQuery = { };
        }
        if (m_pdhProcessCounterPathQuery)
        {
          PdhCloseQuery(m_pdhProcessCounterPathQuery);
          m_pdhProcessCounterPathQuery = {};
        }
      }

      bool Initialize(const InitParams& initParams)
      {
        m_useCpuUtilizationGlobal = initParams.useGlobalCPUUtilization;

        m_pid = GetCurrentProcessId();
        m_selfPidString = std::format(L"pid_{}", m_pid);

        auto status = PdhOpenQueryW(NULL, 0, &m_pdhQuery);
        if (status != ERROR_SUCCESS)
        {
          return false;
        }
        status = PdhOpenQueryW(NULL, 0, &m_pdhProcessCounterPathQuery);
        if (status != ERROR_SUCCESS)
        {
          return false;
        }

        SetupCounterGpuUsage();
        SetupCounterGpuDedicatedMemory();
        SetupCounterCpuUsage();

        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        m_logicalProcessorCount = sysInfo.dwNumberOfProcessors;
        m_cpuCoresUsage.resize(sysInfo.dwNumberOfProcessors);

        m_workBuffer.resize(4096);
        m_exit = false;
        m_workerThread = std::thread([this] { this->WorkerThread(); });
        return true;
      }

      double GetGPUEngineUtilization(const std::wstring& engineName)
      {
        if (auto itr = m_gpuEngineUtilization.find(engineName); itr != m_gpuEngineUtilization.end())
        {
          return itr->second;
        }
        return 0.0;
      }
      void GetGPUEngineUtilization(std::vector<std::wstring>& nameList)
      {
        nameList.clear();
        for (auto& itr : m_gpuEngineUtilization)
        {
          nameList.push_back(itr.first);
        }
      }
      uint64_t GetUsedGPUDedicatedMemory()
      {
        std::unique_lock lock(m_mutex);
        return m_gpuMemory.dedicated;
      }
      uint64_t GetUsedGPUSharedMemory()
      {
        std::unique_lock lock(m_mutex);
        return m_gpuMemory.shared;
      }
      double GetCPUUtilization()
      {
        std::unique_lock lock(m_mutex);
        if (m_useCpuUtilizationGlobal)
        {
          return m_cpuUsageGlobal;
        }
        return m_cpuUsage;
      }
      std::vector<double> GetCPUCoresUtilization()
      {
        std::unique_lock lock(m_mutex);
        return m_cpuCoresUsage;
      }

    private:
      std::thread m_workerThread;
      std::wstring m_selfPidString;
      std::atomic<bool> m_exit;
      std::mutex m_mutex;
      std::condition_variable m_condVar;
      DWORD m_pid = 0xFFFFFFFFu;
      DWORD m_logicalProcessorCount = 0;

      std::chrono::milliseconds m_intervalPeriod = 100ms;
      PDH_HQUERY m_pdhQuery;
      PDH_HCOUNTER m_hGpuDedicateMem, m_hGpuSharedMem;
      PDH_HCOUNTER m_hGpuUsage;
      PDH_HCOUNTER m_hCpuUsage;
      PDH_HCOUNTER m_hCpuUsageGlobal;

      PDH_HQUERY m_pdhProcessCounterPathQuery;
      std::vector<uint8_t> m_workBuffer;

      bool m_useCpuUtilizationGlobal = true;

      // ---- 以下メンバは、ロック取得して短時間で反映する変数群.
      std::unordered_map<std::wstring, double> m_gpuEngineUtilization;
      struct GPUMemory
      {
        uint64_t dedicated;
        uint64_t shared;
      } m_gpuMemory;
      double m_cpuUsage = 0;
      double m_cpuUsageGlobal = 0;
      std::vector<double> m_cpuCoresUsage;
      // ------------------
    private:

      void SetupCounterGpuDedicatedMemory()
      {
        auto status = PdhAddCounterW(m_pdhQuery, LR"(\GPU Process Memory(*)\Dedicated Usage)", 0, &m_hGpuDedicateMem);
        if (status != ERROR_SUCCESS)
        {
          m_hGpuDedicateMem = {};
        }
        status = PdhAddCounterW(m_pdhQuery, LR"(\GPU Process Memory(*)\Shared Usage)", 0, &m_hGpuSharedMem);
        if (status != ERROR_SUCCESS)
        {
          m_hGpuSharedMem = {};
        }
      }
      void SetupCounterGpuUsage()
      {
        auto status = PdhAddCounterW(m_pdhQuery, LR"(\GPU Engine(*)\Utilization Percentage)", 0, &m_hGpuUsage);
        if (status != ERROR_SUCCESS)
        {
          m_hGpuUsage = {};
        }
      }
      void SetupCounterCpuUsage()
      {
        // Windows11 タスクマネージャーの値と合う値を得るには、"(\Processor Information(_Total)\% Processor Utility)" を使う.
        //  こちらのカウンタは、クロック周波数が状況に応じて変化する CPU に対応したものとなっている.
        //  100% を超える値が取得できることもあるが、タスクマネージャーでは100%にクリップしているそう.
        auto status = PdhAddCounterW(m_pdhQuery, LR"(\Processor Information(*)\% Processor Utility)", 0, &m_hCpuUsageGlobal);
        if (status != ERROR_SUCCESS)
        {
          m_hCpuUsageGlobal = { };
        }

        auto counterPath = GetPathMatched(GetProcessPathList());
        if (counterPath.empty())
        {
          return;
        }
        status = PdhAddCounterW(m_pdhQuery, counterPath.c_str(), 0, &m_hCpuUsage);
        if (status != ERROR_SUCCESS)
        {
          m_hCpuUsage = { };
        }
      }

      std::unordered_map<std::wstring, double> CollectGPUUtilization()
      {
        std::unordered_map<std::wstring, double> gpuEngineUtilization;
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArrayW(m_hGpuUsage, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);

        if (m_workBuffer.size() < bufferSize)
        {
          m_workBuffer.resize(bufferSize);
        }

        auto pdhItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM*>(m_workBuffer.data());
        auto pdhStatus = PdhGetFormattedCounterArrayW(m_hGpuUsage, PDH_FMT_DOUBLE, &bufferSize, &itemCount, pdhItems);
        if (pdhStatus == ERROR_SUCCESS)
        {
          for (DWORD i = 0; i < itemCount; ++i)
          {
            auto name = std::wstring(pdhItems[i].szName);
            // 目的のPIDをフィルタリング.
            if (name.find(m_selfPidString) == name.npos)
            {
              continue;
            }

            // エンジンタイプ部の抽出. "_engtype_" の後ろを取得
            auto keywordEngType = std::wstring(L"_engtype_");
            size_t pos = name.find(keywordEngType);
            auto engineType = std::wstring(name.begin() + pos + keywordEngType.length(), name.end());
            if (!engineType.empty())
            {
              gpuEngineUtilization[engineType] += pdhItems[i].FmtValue.doubleValue;
            }
          }
        }
        return std::move(gpuEngineUtilization);
      }

      uint64_t CollectGPUMemoryDedicated()
      {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        uint64_t dedicatedMemAmount = 0;
        PdhGetFormattedCounterArrayW(m_hGpuDedicateMem, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr);
        if (m_workBuffer.size() < bufferSize)
        {
          m_workBuffer.resize(bufferSize);
        }

        auto pdhItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM*>(m_workBuffer.data());
        auto pdhStatus = PdhGetFormattedCounterArrayW(m_hGpuDedicateMem, PDH_FMT_LARGE, &bufferSize, &itemCount, pdhItems);
        if (pdhStatus == ERROR_SUCCESS)
        {
          for (DWORD i = 0; i < itemCount; ++i)
          {
            auto name = std::wstring(pdhItems[i].szName);
            if (name.find(m_selfPidString) == name.npos)
            {
              continue;
            }

            auto value = pdhItems[i].FmtValue.largeValue;
            dedicatedMemAmount += value;
          }
        }
        return dedicatedMemAmount;
      }
      uint64_t CollectGPUMemoryShared()
      {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        uint64_t sharedMemAmount = 0;
        PdhGetFormattedCounterArrayW(m_hGpuSharedMem, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr);
        if (m_workBuffer.size() < bufferSize)
        {
          m_workBuffer.resize(bufferSize);
        }

        auto pdhItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM*>(m_workBuffer.data());
        auto pdhStatus = PdhGetFormattedCounterArrayW(m_hGpuSharedMem, PDH_FMT_LARGE, &bufferSize, &itemCount, pdhItems);
        if (pdhStatus == ERROR_SUCCESS)
        {
          for (DWORD i = 0; i < itemCount; ++i)
          {
            auto name = std::wstring(pdhItems[i].szName);
            if (name.find(m_selfPidString) == name.npos)
            {
              continue;
            }

            auto value = pdhItems[i].FmtValue.largeValue;
            sharedMemAmount += value;
          }
        }
        return sharedMemAmount;
      }

      // PIDを元に検索するためのパスを作成.
      std::vector<std::wstring> GetProcessPathList()
      {
        std::vector<std::wstring> counterPathList;
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, m_pid);
        std::wstring targetProcessName;
        targetProcessName.resize(MAX_PATH);
        ::GetModuleBaseNameW(hProcess, NULL, targetProcessName.data(), DWORD(targetProcessName.size()));
        targetProcessName = targetProcessName.substr(0, targetProcessName.find_last_of(L'.'));
        {
          auto queryString = std::format(L"\\Process({}*)\\ID Process", targetProcessName);
          LONG value = 0;
          DWORD pathListLength = 0;
          PdhExpandWildCardPathW(NULL, queryString.c_str(), nullptr, &pathListLength, 0);
          pathListLength += 1024; // 予備バッファ.
          std::vector<wchar_t> wszPathListBuffer(pathListLength);
          PdhExpandWildCardPathW(NULL, queryString.c_str(), wszPathListBuffer.data(), &pathListLength, 0);

          auto buffer = wszPathListBuffer.data();
          size_t start = 0;
          while (wszPathListBuffer[start] != L'\0')
          {
            size_t end = start;
            while (wszPathListBuffer[end] != L'\0')
            {
              end++;
            }

            auto namePath = std::wstring(buffer + start, buffer + end);
            if (auto pos = namePath.find(targetProcessName); pos != std::wstring::npos)
            {
              counterPathList.push_back(namePath);
            }
            start = end + 1;
          }
        }
        CloseHandle(hProcess);
        return counterPathList;
      }

      // パスのリストから設定したPIDと合うものを見つけ、プロセッサ時間を求めるカウンタパスを返す.
      std::wstring GetPathMatched(const std::vector<std::wstring>& pathList)
      {
        std::wstring desiredInstancePath;
        for (auto& counterPath : pathList)
        {
          PDH_HCOUNTER hCounter = 0;

          auto status = PdhAddCounterW(m_pdhProcessCounterPathQuery, counterPath.c_str(), 0, &hCounter);
          PdhCollectQueryData(m_pdhProcessCounterPathQuery);

          PDH_FMT_COUNTERVALUE counterValue;
          status = PdhGetFormattedCounterValue(hCounter, PDH_FMT_LONG, nullptr, &counterValue);
          if (status == ERROR_SUCCESS)
          {
            auto PID = counterValue.longValue;
            if (m_pid == PID)
            {
              desiredInstancePath = counterPath;
            }
          }
          PdhRemoveCounter(hCounter);
          if (!desiredInstancePath.empty())
          {
            break;
          }
        }
        auto pos = desiredInstancePath.rfind(L"\\");
        desiredInstancePath = desiredInstancePath.substr(0, pos) + L"\\% Processor Time";
        return desiredInstancePath;
      }

      double CollectCPUUsage()
      {
        double cpuUsage = 0;
        PDH_FMT_COUNTERVALUE counter_value;
        auto status = PdhGetFormattedCounterValue(m_hCpuUsage, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, 0, &counter_value);
        auto result = counter_value.doubleValue;
        if (status == ERROR_SUCCESS)
        {
          cpuUsage = result / m_logicalProcessorCount;
        }
        return cpuUsage;
      }

      std::vector<double> CollectCPUUsageGlobal()
      {
        std::vector<double> cpuCoresUsage;
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArrayW(m_hCpuUsageGlobal, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
        if (m_workBuffer.size() < bufferSize)
        {
          m_workBuffer.resize(bufferSize);
        }
        auto pdhItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM*>(m_workBuffer.data());
        auto pdhStatus = PdhGetFormattedCounterArrayW(m_hCpuUsageGlobal, PDH_FMT_DOUBLE, &bufferSize, &itemCount, pdhItems);
        if (pdhStatus == ERROR_SUCCESS)
        {
          cpuCoresUsage.resize(itemCount);
          for (DWORD i = 0; i < itemCount; ++i)
          {
            uint32_t cpuIndex = 0, coreIndex = 0;
            auto name = std::wstring(pdhItems[i].szName);
            ::swscanf_s(name.c_str(), L"%u,%u", &cpuIndex, &coreIndex);
            auto value = pdhItems[i].FmtValue.doubleValue;
            if (coreIndex < cpuCoresUsage.size())
            {
              cpuCoresUsage[coreIndex] = value;
            }
          }
        }
        return cpuCoresUsage;
      }

      void WorkerThread()
      {
        while (!m_exit)
        {
          PDH_STATUS pdhStatus;
          pdhStatus = PdhCollectQueryData(m_pdhQuery);
          if (pdhStatus != ERROR_SUCCESS)
          {
            continue;
          }

          // GPU Engine Utilization
          auto gpuEngineUtilization = CollectGPUUtilization();

          // GPU Memory
          auto gpuDedicatedMem = CollectGPUMemoryDedicated();
          auto gpuSharedMem = CollectGPUMemoryShared();

          // CPU
          double cpuUsage = m_cpuUsage;
          double cpuUsageGlobal = m_cpuUsageGlobal;
          auto cpuCoresUsage = CollectCPUUsageGlobal();
          
          cpuUsageGlobal = 0;
          for (auto& usage : cpuCoresUsage)
          {
            cpuUsageGlobal += usage;
          }
          cpuUsageGlobal /= (std::max)(1ull, cpuCoresUsage.size());
          cpuUsageGlobal = (std::min)(cpuUsageGlobal, 100.0);

          if(!m_useCpuUtilizationGlobal)
          {
            auto processCpuUsagePathList = GetProcessPathList();
            if (processCpuUsagePathList.size() > 1)
            {
              // 同名プロセスが増減の債には、カウンターパスの再計算が必要.
              auto counterPath = GetPathMatched(processCpuUsagePathList);
              if (!counterPath.empty())
              {
                // カウンタを再登録する.
                PdhRemoveCounter(m_hCpuUsage);
                PdhAddCounter(m_pdhQuery, counterPath.c_str(), 0, &m_hCpuUsage);
              }
            }
            else
            {
              cpuUsage = CollectCPUUsage();
            }
          }

          // データを反映するためロックを取得.
          {
            std::unique_lock lock(m_mutex);
            m_cpuUsage = (m_cpuUsage + cpuUsage) * 0.5;
            m_cpuUsageGlobal = cpuUsageGlobal;
            m_gpuEngineUtilization = gpuEngineUtilization;
            m_gpuMemory.dedicated = gpuDedicatedMem;
            m_gpuMemory.shared = gpuSharedMem;

            m_cpuCoresUsage.resize(cpuCoresUsage.size());
            for (uint32_t i = 0; i < cpuCoresUsage.size(); ++i)
            {
              m_cpuCoresUsage[i] = (std::min)(cpuCoresUsage[i], 100.0);
            }
          }

          std::this_thread::sleep_for(m_intervalPeriod);
        }
      }
    };

    std::unique_ptr<SimplePerfCounter> gPerformanceCounter;
  }

  // ************************
  // Public Functions
  // ************************
  bool Initialize(const InitParams& initParams)
  {
    if (impl::gPerformanceCounter)
    {
      return true; // 既に初期化されている.
    }
    impl::gPerformanceCounter = std::make_unique<impl::SimplePerfCounter>();
    return impl::gPerformanceCounter->Initialize(initParams);
  }

  void Shutdown()
  {
    if (impl::gPerformanceCounter)
    {
      impl::gPerformanceCounter.reset();
    }
  }

  uint64_t GetUsedGPUDedicatedMemory()
  {
    if (impl::gPerformanceCounter)
    {
      return impl::gPerformanceCounter->GetUsedGPUDedicatedMemory();
    }
    return 0;
  }

  uint64_t GetUsedGPUSharedMemory()
  {
    if (impl::gPerformanceCounter)
    {
      return impl::gPerformanceCounter->GetUsedGPUSharedMemory();
    }
    return 0;
  }

  double GetGPUEngineUtilization()
  {
    if (impl::gPerformanceCounter)
    {
      return impl::gPerformanceCounter->GetGPUEngineUtilization(L"3D");
    }
    return 0;
  }
  double GetGPUEngineUtilization(const wchar_t* engineName)
  {
    if (engineName == nullptr)
    {
      return 0;
    }
    if (impl::gPerformanceCounter)
    {
      return impl::gPerformanceCounter->GetGPUEngineUtilization(engineName);
    }
    return 0;
  }
  std::vector<std::wstring> GetGPUEngineNames()
  {
    std::vector<std::wstring> nameList;
    if (impl::gPerformanceCounter)
    {
      impl::gPerformanceCounter->GetGPUEngineUtilization(nameList);
    }
    return nameList;
  }

  double GetCPUUtilization()
  {
    if (impl::gPerformanceCounter)
    {
      return impl::gPerformanceCounter->GetCPUUtilization();
    }
    return 0;
  }
  std::vector<double> GetCPUCoresUtilization()
  {
    if (impl::gPerformanceCounter)
    {
      return impl::gPerformanceCounter->GetCPUCoresUtilization();
    }
    return std::vector<double>();
  }

} // tiny_perf_counter
#endif // TINY_PERFORMANCE_COUNTER_IMPLEMENTATION

#endif // TINY_PERFORMANCE_COUNTER_H

