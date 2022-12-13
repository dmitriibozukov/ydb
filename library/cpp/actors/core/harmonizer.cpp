#include "harmonizer.h"

#include "probes.h"
#include "actorsystem.h"

#include <library/cpp/actors/util/cpu_load_log.h>
#include <library/cpp/actors/util/datetime.h>
#include <library/cpp/actors/util/intrinsics.h>

#include <util/system/spinlock.h>

#include <algorithm>

namespace NActors {

LWTRACE_USING(ACTORLIB_PROVIDER);

constexpr bool CheckBinaryPower(ui64 value) {
    return !(value & (value - 1));
}

struct TValueHistory {
    static constexpr ui64 HistoryBufferSize = 8;
    static_assert(CheckBinaryPower(HistoryBufferSize));

    double History[HistoryBufferSize] = {0.0};
    ui64 HistoryIdx = 0;
    ui64 LastTs = Max<ui64>();
    double LastUs = 0.0;
    double AccumulatedUs = 0.0;
    ui64 AccumulatedTs = 0;

    template <bool WithTail=false>
    double GetAvgPartForLastSeconds(ui8 seconds) {
        double sum = AccumulatedUs;
        size_t idx = HistoryIdx;
        ui8 leftSeconds = seconds;
        do {
            idx--;
            leftSeconds--;
            if (idx >= HistoryBufferSize) {
                idx = HistoryBufferSize - 1;
            }
            if (WithTail || leftSeconds) {
                sum += History[idx];
            } else {
                ui64 tsInSecond = Us2Ts(1'000'000.0);
                sum += History[idx] * (tsInSecond - AccumulatedTs) / tsInSecond;
            }
        } while (leftSeconds);
        double duration = 1'000'000.0 * seconds + (WithTail ? Ts2Us(AccumulatedTs): 0.0);
        double avg = sum / duration;
        return avg;
    }

    double GetAvgPart() {
        return GetAvgPartForLastSeconds<true>(HistoryBufferSize);
    }

    void Register(ui64 ts, double valueUs) {
        if (ts < LastTs) {
            LastTs = ts;
            LastUs = valueUs;
            AccumulatedUs = 0.0;
            AccumulatedTs = 0;
            return;
        }
        ui64 lastTs = std::exchange(LastTs, ts);
        ui64 dTs = ts - lastTs;
        double lastUs = std::exchange(LastUs, valueUs);
        double dUs = valueUs - lastUs;
        LWPROBE(RegisterValue, ts, lastTs, dTs, Us2Ts(8'000'000.0), valueUs, lastUs, dUs);

        if (dTs > Us2Ts(8'000'000.0)) {
            dUs = dUs * 1'000'000.0 / Ts2Us(dTs);
            for (size_t idx = 0; idx < HistoryBufferSize; ++idx) {
                History[idx] = dUs;
            }
            AccumulatedUs = 0.0;
            AccumulatedTs = 0;
            return;
        }

        while (dTs > 0) {
            if (AccumulatedTs + dTs < Us2Ts(1'000'000.0)) {
                AccumulatedTs += dTs;
                AccumulatedUs += dUs;
                break;
            } else {
                ui64 addTs = Us2Ts(1'000'000.0) - AccumulatedTs;
                double addUs = dUs * addTs / dTs;
                dTs -= addTs;
                dUs -= addUs;
                History[HistoryIdx] = AccumulatedUs + addUs;
                HistoryIdx = (HistoryIdx + 1) % HistoryBufferSize;
                AccumulatedUs = 0.0;
                AccumulatedTs = 0;
            }
        }
    }
};

struct TThreadInfo {
    TValueHistory Consumed;
    TValueHistory Booked;
};

struct TPoolInfo {
    std::vector<TThreadInfo> ThreadInfo;
    IExecutorPool* Pool = nullptr;
    i16 DefaultThreadCount = 0;
    i16 MinThreadCount = 0;
    i16 MaxThreadCount = 0;
    i16 Priority = 0;
    NMonitoring::TDynamicCounters::TCounterPtr AvgPingCounter;
    NMonitoring::TDynamicCounters::TCounterPtr AvgPingCounterWithSmallWindow;
    ui32 MaxAvgPingUs = 0;
    ui64 LastUpdateTs = 0;

    bool IsBeingStopped(i16 threadIdx);
    double GetBooked(i16 threadIdx);
    double GetlastSecondPoolBooked(i16 threadIdx);
    double GetConsumed(i16 threadIdx);
    double GetlastSecondPoolConsumed(i16 threadIdx);
    void PullStats(ui64 ts);
    i16 GetThreadCount();
    void SetThreadCount(i16 threadCount);
    bool IsAvgPingGood();
};

bool TPoolInfo::IsBeingStopped(i16 threadIdx) {
    return Pool->IsThreadBeingStopped(threadIdx);
}

double TPoolInfo::GetBooked(i16 threadIdx) {
    if ((size_t)threadIdx < ThreadInfo.size()) {
        return ThreadInfo[threadIdx].Booked.GetAvgPart();
    }
    return 0.0;
}

double TPoolInfo::GetlastSecondPoolBooked(i16 threadIdx) {
    if ((size_t)threadIdx < ThreadInfo.size()) {
        return ThreadInfo[threadIdx].Booked.GetAvgPartForLastSeconds(1);
    }
    return 0.0;
}

double TPoolInfo::GetConsumed(i16 threadIdx) {
    if ((size_t)threadIdx < ThreadInfo.size()) {
        return ThreadInfo[threadIdx].Consumed.GetAvgPart();
    }
    return 0.0;
}

double TPoolInfo::GetlastSecondPoolConsumed(i16 threadIdx) {
    if ((size_t)threadIdx < ThreadInfo.size()) {
        return ThreadInfo[threadIdx].Consumed.GetAvgPartForLastSeconds(1);
    }
    return 0.0;
}

#define UNROLL_HISTORY(history) (history)[0], (history)[1], (history)[2], (history)[3], (history)[4], (history)[5], (history)[6], (history)[7]
void TPoolInfo::PullStats(ui64 ts) {
    for (i16 threadIdx = 0; threadIdx < MaxThreadCount; ++threadIdx) {
        TThreadInfo &threadInfo = ThreadInfo[threadIdx];
        threadInfo.Consumed.Register(ts, Pool->GetThreadConsumedUs(threadIdx));
        LWPROBE(SavedValues, Pool->PoolId, Pool->GetName(), "consumed", UNROLL_HISTORY(threadInfo.Consumed.History));
        threadInfo.Booked.Register(ts, Pool->GetThreadBookedUs(threadIdx));
        LWPROBE(SavedValues, Pool->PoolId, Pool->GetName(), "booked", UNROLL_HISTORY(threadInfo.Booked.History));
    }
}
#undef UNROLL_HISTORY

i16 TPoolInfo::GetThreadCount() {
    return Pool->GetThreadCount();
}

void TPoolInfo::SetThreadCount(i16 threadCount) {
    Pool->SetThreadCount(threadCount);
}

bool TPoolInfo::IsAvgPingGood() {
    bool res = true;
    if (AvgPingCounter) {
        res &= *AvgPingCounter > MaxAvgPingUs;
    }
    if (AvgPingCounterWithSmallWindow) {
        res &= *AvgPingCounterWithSmallWindow > MaxAvgPingUs;
    }
    return res;
}

class THarmonizer: public IHarmonizer {
private:
    std::atomic<bool> IsDisabled = false;
    TSpinLock Lock;
    std::atomic<ui64> NextHarmonizeTs = 0;
    std::vector<TPoolInfo> Pools;
    std::vector<ui16> PriorityOrder;

    void PullStats(ui64 ts);
    void HarmonizeImpl(ui64 ts);
    void CalculatePriorityOrder();
public:
    THarmonizer(ui64 ts);
    virtual ~THarmonizer();
    double Rescale(double value) const;
    void Harmonize(ui64 ts) override;
    void DeclareEmergency(ui64 ts) override;
    void AddPool(IExecutorPool* pool, TSelfPingInfo *pingInfo) override;
    void Enable(bool enable) override;
};

THarmonizer::THarmonizer(ui64 ts) {
    NextHarmonizeTs = ts;
}

THarmonizer::~THarmonizer() {
}

double THarmonizer::Rescale(double value) const {
  return Max(0.0, Min(1.0, value * (1.0/0.9)));
}

void THarmonizer::PullStats(ui64 ts) {
    for (TPoolInfo &pool : Pools) {
        pool.PullStats(ts);
    }
}

Y_FORCE_INLINE bool IsStarved(double consumed, double booked) {
    return Max(consumed, booked) > 0.1 && consumed < booked * 0.7;
}

Y_FORCE_INLINE bool IsHoggish(double booked, ui16 currentThreadCount) {
    return booked < currentThreadCount - 1;
}

void THarmonizer::HarmonizeImpl(ui64 ts) {
    bool isStarvedPresent = false;
    double booked = 0.0;
    double consumed = 0.0;
    double lastSecondBooked = 0.0;
    i64 beingStopped = 0;
    i64 total = 0;
    TStackVec<size_t, 8> needyPools;
    TStackVec<size_t, 8> hoggishPools;
    for (size_t poolIdx = 0; poolIdx < Pools.size(); ++poolIdx) {
        TPoolInfo& pool = Pools[poolIdx];
        total += pool.DefaultThreadCount;
        double poolBooked = 0.0;
        double poolConsumed = 0.0;
        double lastSecondPoolBooked = 0.0;
        double lastSecondPoolConsumed = 0.0;
        beingStopped += pool.Pool->GetBlockingThreadCount();
        for (i16 threadIdx = 0; threadIdx < pool.MaxThreadCount; ++threadIdx) {
            poolBooked += Rescale(pool.GetBooked(threadIdx));
            lastSecondPoolBooked += Rescale(pool.GetlastSecondPoolBooked(threadIdx));
            poolConsumed += Rescale(pool.GetConsumed(threadIdx));
            lastSecondPoolConsumed += Rescale(pool.GetlastSecondPoolConsumed(threadIdx));
        }
        bool isStarved = IsStarved(consumed, booked) || IsStarved(lastSecondPoolConsumed, lastSecondPoolBooked);
        if (isStarved) {
            isStarvedPresent = true;
        }
        ui32 currentThreadCount = pool.GetThreadCount();
        bool isNeedy = pool.IsAvgPingGood() && poolBooked >= currentThreadCount;
        if (pool.AvgPingCounter) {
            if (pool.LastUpdateTs + Us2Ts(3'000'000ull) > ts) {
                isNeedy = false;
            } else {
                pool.LastUpdateTs = ts;
            }
        }
        if (isNeedy) {
            needyPools.push_back(poolIdx);
        }
        bool isHoggish = IsHoggish(poolBooked, currentThreadCount)
                || IsHoggish(lastSecondPoolBooked, currentThreadCount);
        if (isHoggish) {
            hoggishPools.push_back(poolIdx);
        }
        booked += poolBooked;
        consumed += poolConsumed;
        LWPROBE(HarmonizeCheckPool, poolIdx, pool.Pool->GetName(), poolBooked, poolConsumed, lastSecondPoolBooked, lastSecondPoolConsumed, pool.GetThreadCount(), pool.MaxThreadCount, isStarved, isNeedy, isHoggish);
    }
    double budget = total - Max(booked, lastSecondBooked);
    if (budget < -0.1) {
        isStarvedPresent = true;
    }
    double overbooked = consumed - booked;
    if (isStarvedPresent) {
      // last_starved_at_consumed_value = сумма по всем пулам consumed;
      // TODO(cthulhu): использовать как лимит планвно устремлять этот лимит к total,
      // использовать вместо total
      if (beingStopped && beingStopped >= overbooked) {
          // do nothing
      } else {
          TStackVec<size_t> reorder;
          for (size_t i = 0; i < Pools.size(); ++i) {
              reorder.push_back(i);
          }
          for (ui16 poolIdx : PriorityOrder) {
              TPoolInfo &pool = Pools[poolIdx];
              i64 threadCount = pool.GetThreadCount();
              if (threadCount > pool.DefaultThreadCount) {
                  pool.SetThreadCount(threadCount - 1);
                  overbooked--;
                  LWPROBE(HarmonizeOperation, poolIdx, pool.Pool->GetName(), "decrease", threadCount - 1, pool.DefaultThreadCount, pool.MaxThreadCount);
                  if (overbooked < 1) {
                      break;
                  }
              }
          }
      }
    } else {
        for (size_t needyPoolIdx : needyPools) {
            TPoolInfo &pool = Pools[needyPoolIdx];
            if (budget >= 1.0) {
                i64 threadCount = pool.GetThreadCount();
                if (threadCount + 1 <= pool.MaxThreadCount) {
                    pool.SetThreadCount(threadCount + 1);
                    budget -= 1.0;
                    LWPROBE(HarmonizeOperation, needyPoolIdx, pool.Pool->GetName(), "increase", threadCount + 1, pool.DefaultThreadCount, pool.MaxThreadCount);
                }
            }
        }
    }
    for (size_t hoggishPoolIdx : hoggishPools) {
        TPoolInfo &pool = Pools[hoggishPoolIdx];
        i64 threadCount = pool.GetThreadCount();
        if (threadCount > pool.MinThreadCount) {
            LWPROBE(HarmonizeOperation, hoggishPoolIdx, pool.Pool->GetName(), "decrease", threadCount - 1, pool.DefaultThreadCount, pool.MaxThreadCount);
            pool.SetThreadCount(threadCount - 1);
        }
    }
}

void THarmonizer::CalculatePriorityOrder() {
    PriorityOrder.resize(Pools.size());
    Iota(PriorityOrder.begin(), PriorityOrder.end(), 0);
    Sort(PriorityOrder.begin(), PriorityOrder.end(), [&] (i16 lhs, i16 rhs) {
        if (Pools[lhs].Priority != Pools[rhs].Priority)  {
            return Pools[lhs].Priority < Pools[rhs].Priority;
        }
        return Pools[lhs].Pool->PoolId > Pools[rhs].Pool->PoolId;
    });
}

void THarmonizer::Harmonize(ui64 ts) {
    if (IsDisabled || NextHarmonizeTs > ts || !Lock.TryAcquire()) {
        LWPROBE(TryToHarmonizeFailed, ts, NextHarmonizeTs, IsDisabled, false);
        return;
    }
    // Check again under the lock
    if (IsDisabled) {
        LWPROBE(TryToHarmonizeFailed, ts, NextHarmonizeTs, IsDisabled, true);
        Lock.Release();
        return;
    }
    // Will never reach this line disabled
    ui64 previousNextHarmonizeTs = NextHarmonizeTs.exchange(ts + Us2Ts(1'000'000ull));
    LWPROBE(TryToHarmonizeSuccess, ts, NextHarmonizeTs, previousNextHarmonizeTs);

    if (PriorityOrder.empty()) {
        CalculatePriorityOrder();
    }

    PullStats(ts);
    HarmonizeImpl(ts);

    Lock.Release();
}

void THarmonizer::DeclareEmergency(ui64 ts) {
    NextHarmonizeTs = ts;
}

void THarmonizer::AddPool(IExecutorPool* pool, TSelfPingInfo *pingInfo) {
    TGuard<TSpinLock> guard(Lock);
    TPoolInfo poolInfo;
    poolInfo.Pool = pool;
    poolInfo.DefaultThreadCount = pool->GetDefaultThreadCount();
    poolInfo.MinThreadCount = pool->GetMinThreadCount();
    poolInfo.MaxThreadCount = pool->GetMaxThreadCount();
    poolInfo.ThreadInfo.resize(poolInfo.MaxThreadCount);
    poolInfo.Priority = pool->GetPriority();
    pool->SetThreadCount(poolInfo.DefaultThreadCount);
    if (pingInfo) {
        poolInfo.AvgPingCounter = pingInfo->AvgPingCounter;
        poolInfo.AvgPingCounterWithSmallWindow = pingInfo->AvgPingCounterWithSmallWindow;
        poolInfo.MaxAvgPingUs = pingInfo->MaxAvgPingUs;
    }
    Pools.push_back(poolInfo);
    PriorityOrder.clear();
};

void THarmonizer::Enable(bool enable) {
    TGuard<TSpinLock> guard(Lock);
    IsDisabled = enable;
}

IHarmonizer* MakeHarmonizer(ui64 ts) {
    return new THarmonizer(ts);
}

}