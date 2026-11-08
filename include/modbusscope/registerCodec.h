#pragma once
#include <QVector>
#include <cstring>
#include <QtGlobal>   // quint64 / qint64

namespace modbusscope {

inline quint32 toUint32(const QVector<quint16>& v, int idx, bool highWordFirst) {
    return highWordFirst
        ? (static_cast<quint32>(v[idx]) << 16) | v[idx + 1]
        : (static_cast<quint32>(v[idx + 1]) << 16) | v[idx];
}

inline float toFloat32(const QVector<quint16>& v, int idx, bool highWordFirst) {
    const quint32 raw = toUint32(v, idx, highWordFirst);
    float f;
    std::memcpy(&f, &raw, sizeof(f));
    return f;
}

inline qint32 toI32(const QVector<quint16>& v, int idx, bool highWordFirst) {
    const quint32 raw = toUint32(v, idx, highWordFirst);
    qint32 i;
    std::memcpy(&i, &raw, sizeof(i));
    return i;
}

enum class WordOrder64 {
    ABCD,   // v[0] 最高 16 位 ... v[3] 最低 16 位
    CDAB,   // v[0] 最低 16 位 ... v[3] 最高 16 位
};

// 按字序拼 64 位无符号整数
inline quint64 toUint64(const QVector<quint16>& v, int idx, WordOrder64 order) {
    switch (order) {
    case WordOrder64::ABCD:
        return (static_cast<quint64>(v[idx])     << 48)
             | (static_cast<quint64>(v[idx + 1]) << 32)
             | (static_cast<quint64>(v[idx + 2]) << 16)
             |  static_cast<quint64>(v[idx + 3]);
    case WordOrder64::CDAB:
        return (static_cast<quint64>(v[idx + 3]) << 48)
             | (static_cast<quint64>(v[idx + 2]) << 32)
             | (static_cast<quint64>(v[idx + 1]) << 16)
             |  static_cast<quint64>(v[idx]);
    }
    return 0; // 不可达，仅为消除编译警告
}

// 按字序拼 64 位有符号整数（补码解释）
inline qint64 toI64(const QVector<quint16>& v, int idx, WordOrder64 order) {
    const quint64 raw = toUint64(v, idx, order);
    qint64 i;
    std::memcpy(&i, &raw, sizeof(i));
    return i;
}

} // namespace modbusscope