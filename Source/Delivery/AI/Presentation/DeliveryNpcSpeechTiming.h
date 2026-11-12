#pragma once
#include "CoreMinimal.h"

namespace DeliveryNpcSpeech
{
    // 按播放时间计算本帧需要发送的长度，提前准备 0.4 秒的声音
    inline int32 GetSendBytes(double TimeFromStart,float Duration,int32 TotalBytes,int32 SentBytes)
    {
        const double ExpectedBytes=FMath::Max(0.0,TimeFromStart+.4)*TotalBytes/FMath::Max(.01f,Duration);
        const int32 SendThrough=int32(FMath::Min(double(TotalBytes),ExpectedBytes));
        // 卡顿后分帧补发压缩语音，让受击和移动状态仍有带宽同步
        return FMath::Clamp(SendThrough-SentBytes,0,2048);
    }
}
