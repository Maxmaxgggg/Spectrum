#pragma once
#include <qjsonobject.h>

struct ComputationSettings
{
    enum Algorithm { SimpleXor = 0, GrayCode = 1, DualCode = 2 };
    enum EnumerationType { Full = 0, Partial = 1 };
    enum ComputeDevice { CPU = 0, GPU = 1 };

    Algorithm       algorithmType = SimpleXor;
    EnumerationType enumType = Full;
    int             maxRows = 0;
    ComputeDevice   compDev = CPU;

    struct computeDeviceSettings {
        int threadsCpu = 1;
        int blocksGpu = 1;
        int threadsGpu = 1;
    } compDevSet;
    ComputationSettings() noexcept = default;

    // Конструктор копирования
    ComputationSettings(const ComputationSettings& other) noexcept
        : algorithmType(other.algorithmType)
        , enumType(other.enumType)
        , maxRows(other.maxRows)
        , compDev(other.compDev)
        , compDevSet(other.compDevSet){ }

    // Рекомендуется также явно определить оператор присваивания
    ComputationSettings& operator=(const ComputationSettings& other) noexcept
    {
        if (this == &other) return *this;
        algorithmType = other.algorithmType;
        enumType = other.enumType;
        maxRows = other.maxRows;
        compDev = other.compDev;
        compDevSet = other.compDevSet;
        return *this;
    }
    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["algorithmType"] = static_cast<int>(algorithmType);
        obj["enumType"] = static_cast<int>(enumType);
        obj["maxRows"] = maxRows;
        obj["compDev"] = static_cast<int>(compDev);

        QJsonObject dev;
        dev["threadsCpu"] = compDevSet.threadsCpu;
        dev["blocksGpu"]  = compDevSet.blocksGpu;
        dev["threadsGpu"] = compDevSet.threadsGpu;

        obj["compDevSet"] = dev;

        return obj;
    }

    static ComputationSettings fromJson(const QJsonObject& obj)
    {
        ComputationSettings s;

        s.algorithmType = static_cast<Algorithm>(obj["algorithmType"].toInt());
        s.enumType = static_cast<EnumerationType>(obj["enumType"].toInt());
        s.maxRows = obj["maxRows"].toInt();
        s.compDev = static_cast<ComputeDevice>(obj["compDev"].toInt());

        QJsonObject dev = obj["compDevSet"].toObject();
        s.compDevSet.threadsCpu = dev["threadsCpu"].toInt();
        s.compDevSet.blocksGpu = dev["blocksGpu"].toInt();
        s.compDevSet.threadsGpu = dev["threadsGpu"].toInt();

        return s;
    }
};