#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "te_host.h"

class IsoFs;
namespace title { class Files; class Input; class SaveStorage; }

namespace te {

class Clock {
public:
    explicit Clock(const Settings& settings);
    ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    void Start();
    bool Started() const { return started_; }

    void StopPacer();

    uint64_t Ticks() const;

    uint64_t T0Counts() const;

    uint64_t BoundaryCount() const { return boundaries_; }

    bool LatestFieldEdge(int64_t* edge_ns, uint64_t* field) const;

    static constexpr uint64_t kChargeCapTicks = 576000u / 50u;
    void ChargeInFrame(uint64_t ticks);
    uint64_t InFrameTicks() const { return in_frame_; }

    void Boundary();
    using Hook = std::function<void(uint64_t boundary)>;

    int AddBoundaryHook(const std::string& name, Hook hook);
    void RemoveBoundaryHook(int id);

    struct Civil { int year, month, day, hour, minute, second; };
    Civil LocalTime() const;

    uint64_t Fields() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool started_ = false;
    uint64_t start_counts_ = 0;
    uint64_t boundaries_ = 0;
    uint64_t in_frame_ = 0;
    struct Entry { int id; std::string name; Hook hook; };
    std::vector<Entry> hooks_;
    int next_hook_ = 1;
};

class Disc {
public:
    explicit Disc(const Settings& settings);
    ~Disc();

    int32_t Initialise();
    int32_t SetMedia(int32_t media);

    int32_t WaitReady() const;
    bool IsOpen() const;
    const IsoFs& Iso() const { return *iso_; }
    const std::string& Error() const { return error_; }
    int32_t Media() const { return media_; }

private:
    std::unique_ptr<IsoFs> iso_;
    std::string path_, error_;
    int32_t media_ = 2;
    bool opened_ = false, tried_ = false;
};

class Files {
public:
    explicit Files(Disc& disc);
    ~Files();

    void Reset();
    int32_t Open(const char* path, int32_t flags);
    int32_t Close(int32_t file);
    int32_t Read(int32_t file, uint32_t buffer, int32_t size);
    int32_t Write(int32_t file, uint32_t buffer, int32_t size);
    int32_t Seek(int32_t file, int32_t offset, int32_t whence);
    title::Files& Core() { return *core_; }

private:
    struct Memory;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<title::Files> core_;
};

class Pads {
public:
    static constexpr unsigned kPorts = 2, kSlots = 4;

    struct Sample {
        bool connected = false;
        uint16_t buttons = 0;
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    };

    using Sampler = std::function<Sample(unsigned port, unsigned slot)>;

    using MotorOutput = std::function<bool(unsigned port, unsigned slot, uint8_t small, uint8_t big)>;

    Pads(Clock& clock, const Settings& settings);
    ~Pads();
    void SetSampler(Sampler sampler);

    void SetMotorOutput(MotorOutput output) { motors_ = std::move(output); }

    int32_t Initialise();
    int32_t Open(int32_t port, int32_t slot, uint32_t buffer);
    int32_t Close(int32_t port, int32_t slot);
    int32_t Read(int32_t port, int32_t slot, uint32_t data);
    int32_t GetState(int32_t port, int32_t slot) const;
    int32_t GetRequestState(int32_t port, int32_t slot) const;
    int32_t InfoMode(int32_t port, int32_t slot, int32_t info, int32_t index) const;
    int32_t SetMainMode(int32_t port, int32_t slot, int32_t mode, int32_t lock);
    int32_t InfoActuator(int32_t port, int32_t slot, int32_t actuator, int32_t term) const;
    int32_t SetActuatorAlign(int32_t port, int32_t slot, uint32_t align);
    int32_t SetActuatorDirect(int32_t port, int32_t slot, uint32_t values);
    int32_t InfoPressureMode(int32_t port, int32_t slot) const;
    int32_t EnterPressureMode(int32_t port, int32_t slot);

    void SampleBoundary(uint64_t boundary);
    title::Input& Core() { return *core_; }

private:
    std::unique_ptr<title::Input> core_;
    Sampler sampler_;
    MotorOutput motors_;
    std::array<uint32_t, kPorts * kSlots> buffers_{};
    Clock& clock_;
    int hook_ = 0;
};

class Saves {
public:
    Saves(Clock& clock, const Settings& settings);
    ~Saves();

    int32_t Initialise(uint32_t region, uint32_t product);
    int32_t Update(int32_t port, int32_t slot, uint32_t info);
    void GetResult(uint32_t out, int32_t operation);
    int32_t Format(int32_t port, int32_t slot);
    int32_t CreateSave(int32_t port, int32_t slot, uint32_t save);
    int32_t MeasureSave(int32_t port, int32_t slot, uint32_t save, uint32_t kilobytes);
    int32_t FindFile(int32_t port, int32_t slot, uint32_t file, uint32_t save, uint32_t size, uint32_t entry);
    int32_t Write(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t data, int32_t size);
    int32_t Read(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t buffer, int32_t size);
    static int32_t FileKilobytes(int32_t size);
    static int32_t SaveBytes(int32_t kilobytes, int32_t files);
    title::SaveStorage& Core() { return *core_; }
    bool Started() const { return started_; }

private:
    struct Outputs { uint32_t value = 0, entry = 0, data = 0, size = 0; };
    void Publish();
    std::unique_ptr<title::SaveStorage> core_;
    Clock& clock_;
    Outputs outputs_;
    bool started_ = false;
};

class Services {
public:
    static Services& Get();

    void Initialise();
    void StartServices();
    bool Initialised() const { return initialised_; }
    bool Started() const { return started_; }

    Clock& clock();
    Disc& disc();
    Files& files();
    Pads& pads();
    Saves& saves();

private:
    Services() = default;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<Disc> disc_;
    std::unique_ptr<Files> files_;
    std::unique_ptr<Pads> pads_;
    std::unique_ptr<Saves> saves_;
    bool initialised_ = false, started_ = false;
};

}
