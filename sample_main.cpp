#define TINY_PERFORMANCE_COUNTER_IMPLEMENTATION
#include "tiny_performance_counter.h"

#include <thread>
#include <chrono>
#include <iostream>
#include <conio.h>

std::atomic<bool> gInterrupt = false;
void CheckInput()
{
  _getch();
  gInterrupt = true;
}

int main()
{
  using namespace std;

  // ������.
  tiny_perf_counter::InitParams initParams{};
  initParams.useGlobalCPUUtilization = true;  // �^�X�N�}�l�[�W���[�ł� CPU �g�p���ƍ����`���ō̎悷��.
  tiny_perf_counter::Initialize(initParams);

  // �L�[���͂�҂��A1�b�����ɃJ�E���^��\������.
  cout << "Press any key to exit." << endl;
  auto collectThread = std::thread([]() { CheckInput(); });
  while (!gInterrupt)
  {
    cout << "CPU Usage :    " << tiny_perf_counter::GetCPUUtilization() << " %" << endl;
    for (auto& coreUsage : tiny_perf_counter::GetCPUCoresUtilization())
    {
      cout << " " << int(coreUsage) << " ";
    }
    cout << endl;

    cout << "GPU (3D)   Usage: " << (tiny_perf_counter::GetGPUEngineUtilization()) << " %\n";
    cout << "GPU (Copy) Usage: " << (tiny_perf_counter::GetGPUEngineUtilization(L"Copy")) << " %\n";

    cout << "GPU Dedicated: " << tiny_perf_counter::GetUsedGPUDedicatedMemory() / double(1024 * 1024) << "MB\n";
    cout << "GPU Shared:    " << tiny_perf_counter::GetUsedGPUSharedMemory() / double(1024 * 1024) << "MB\n";
    cout << "\n";

    this_thread::sleep_for(chrono::seconds(1));
  }
  collectThread.join();

  // �I������.
  tiny_perf_counter::Shutdown();

  return 0;
}