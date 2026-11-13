#pragma once

#include "CoreMinimal.h"

enum class EDeliveryRightMouseRelease : uint8 { OtherInput, Click, ViewOrGrab };

// 右键短按用于出拳，按住或拖动用于视角；双键抓取后不再补判为点击
struct FDeliveryRightMouseGesture
{
    static constexpr double ClickSeconds=.2;
    static constexpr double DragDistance=4.0;

    void Begin(double Now)
    {
        StartedAt=Now;
        MouseTravel=0;
        bActive=true;
        bClickBlocked=false;
        LastMovementFrame=MAX_uint64;
    }

    void AddMovement(const FVector2D& Delta)
    {
        if (!bActive) return;
        if (Delta.ContainsNaN()) { bClickBlocked=true; return; }
        MouseTravel+=Delta.Size();
        if (MouseTravel>=DragDistance) bClickBlocked=true;
    }

    // 视角回调和松开回调可能读取同一帧的位移，只累计一次
    void AddMovement(const FVector2D& Delta,uint64 Frame)
    {
        if (LastMovementFrame==Frame) return;
        LastMovementFrame=Frame;
        AddMovement(Delta);
    }

    void BlockClick() { if (bActive) bClickBlocked=true; }
    bool IsActive() const { return bActive; }

    EDeliveryRightMouseRelease Finish(double Now)
    {
        if (!bActive) return EDeliveryRightMouseRelease::OtherInput;
        const double Elapsed=Now-StartedAt;
        const bool bClick=!bClickBlocked && FMath::IsFinite(Elapsed) && Elapsed>=0 && Elapsed<=ClickSeconds;
        Reset();
        return bClick?EDeliveryRightMouseRelease::Click:EDeliveryRightMouseRelease::ViewOrGrab;
    }

    void Reset()
    {
        StartedAt=0;
        MouseTravel=0;
        bActive=false;
        bClickBlocked=false;
        LastMovementFrame=MAX_uint64;
    }

private:
    double StartedAt=0;
    double MouseTravel=0;
    bool bActive=false;
    bool bClickBlocked=false;
    uint64 LastMovementFrame=MAX_uint64;
};
