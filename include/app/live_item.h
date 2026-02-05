#ifndef LIVE_ITEM_H
#define LIVE_ITEM_H

#include <QString>
#include <QDateTime>

// 直播状态枚举
enum class LiveStatus {
    ALL = 0,      // 全部
    PENDING = 1,  // 待开播
    LIVE = 2,     // 直播中
    ENDED = 3     // 已结束
};

// 审核状态枚举
enum class ReviewStatus {
    PENDING = 0,  // 待审核
    APPROVED = 1, // 审核通过
    REJECTED = 2  // 驳回
};

// 直播列表项结构体
struct LiveItem {
    QString liveId;                           // 直播ID
    QString title;                            // 直播标题
    QDateTime createTime;                     // 创建时间
    QDateTime startTime;                      // 开播时间
    QDateTime endTime;                        // 结束时间
    QString pushUrl;                          // 推流地址
    int type = 0;                             // 直播类型
    LiveStatus status = LiveStatus::PENDING;  // 直播状态
    int reserveCount = 0;                     // 预约人数
    int viewCount = 0;                        // 观看人数
    bool isVerticalScreen = false;            // 是否竖屏
};

// 状态转字符串（用于UI显示）
inline QString liveStatusToString(LiveStatus status) {
    switch (status) {
        case LiveStatus::PENDING: return "待开播";
        case LiveStatus::LIVE:    return "直播中";
        case LiveStatus::ENDED:   return "已结束";
        default:                  return "未知";
    }
}

// 状态转接口参数（用于请求筛选）
inline int liveStatusToApiParam(LiveStatus status) {
    return static_cast<int>(status);
}

#endif // LIVE_ITEM_H
