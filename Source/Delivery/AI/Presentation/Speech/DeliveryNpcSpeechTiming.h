#pragma once
#include "CoreMinimal.h"

/// 服务器分段发声音时的节奏计算
namespace DeliveryNpcSpeech
{
    /// @brief 按已经播了多久，算出这一帧该发多少字节，比播放进度提前 0.4 秒
    /// 一帧最多 2048 字节，卡顿后分几帧补，给受击和移动的同步留出带宽
    inline int32 GetSendBytes(double TimeFromStart /* 这句已经播了几秒 */,float Duration /* 这句总共几秒 */,int32 TotalBytes /* 这句压缩后共多少字节 */,int32 SentBytes /* 已经发出多少字节 */)
    {
        const double ExpectedBytes=FMath::Max(0.0,TimeFromStart+.4)*TotalBytes/FMath::Max(.01f,Duration);
        const int32 SendThrough=int32(FMath::Min(double(TotalBytes),ExpectedBytes));
        return FMath::Clamp(SendThrough-SentBytes,0,2048);
    }
}
