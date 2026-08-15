#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Passive MPEG-TS continuity diagnostics used by v152. This helper never
// modifies packet bytes. It follows payload-aware continuity semantics:
// payload packets advance CC, adaptation-only packets do not, an explicit
// discontinuity_indicator resets the continuity expectation, and byte-identical
// repeated payload packets are counted separately as exact duplicates.
class TsCcStageTrace {
public:
    TsCcStageTrace() = default;
    TsCcStageTrace(std::string streamId, std::string stage)
        : streamId_(std::move(streamId)), stage_(std::move(stage)) {}

    void configure(std::string streamId, std::string stage) {
        streamId_ = std::move(streamId);
        stage_ = std::move(stage);
    }

    void inspect(const uint8_t* data, std::size_t size) {
        if (!data || size == 0) return;
        ++calls_;
        lastBufferSize_ = size;
        if ((size % kPacketSize) != 0) ++unalignedBuffers_;

        // Fast path for the normal TVStreammerSAT5 case (whole 188-byte TS
        // packets). This keeps diagnostic overhead negligible in PRE_SEND and
        // avoids allocating a vector for every UDP datagram.
        if (remainder_.empty() && (size % kPacketSize) == 0 && data[0] == 0x47) {
            for (std::size_t offset = 0; offset < size; offset += kPacketSize) {
                inspectPacket(data + offset);
            }
            maybeLog();
            return;
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(remainder_.size() + size);
        bytes.insert(bytes.end(), remainder_.begin(), remainder_.end());
        bytes.insert(bytes.end(), data, data + size);
        remainder_.clear();

        std::size_t offset = findAlignment(bytes.data(), bytes.size());
        if (offset == npos) {
            ++alignmentMisses_;
            keepTail(bytes, 0);
            maybeLog();
            return;
        }
        if (offset != 0) {
            resyncBytes_ += offset;
        }

        for (; offset + kPacketSize <= bytes.size(); offset += kPacketSize) {
            inspectPacket(bytes.data() + offset);
        }
        if (offset < bytes.size()) keepTail(bytes, offset);
        maybeLog();
    }

private:
    static constexpr std::size_t kPacketSize = 188;
    static constexpr std::size_t kPidCount = 8192;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static uint64_t packetHash(const uint8_t* packet) {
        uint64_t hash = 1469598103934665603ULL;
        for (std::size_t i = 0; i < kPacketSize; ++i) {
            hash ^= packet[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static std::size_t findAlignment(const uint8_t* data, std::size_t size) {
        if (!data || size < kPacketSize) return npos;
        const std::size_t maxOffset = std::min<std::size_t>(kPacketSize, size);
        for (std::size_t candidate = 0; candidate < maxOffset; ++candidate) {
            if (data[candidate] != 0x47) continue;
            if (candidate + kPacketSize >= size || data[candidate + kPacketSize] == 0x47) {
                return candidate;
            }
        }
        return npos;
    }

    void keepTail(const std::vector<uint8_t>& bytes, std::size_t offset) {
        if (offset >= bytes.size()) return;
        const std::size_t available = bytes.size() - offset;
        const std::size_t keep = std::min<std::size_t>(available, kPacketSize * 4 - 1);
        remainder_.assign(bytes.end() - keep, bytes.end());
    }

    void recordCcError(uint16_t pid, const char* type, uint8_t previous,
                       uint8_t current, uint8_t expected, uint8_t afc,
                       bool hasPayload, bool pusi, bool discontinuity) {
        ++ccErrors_;
        ++pidErrors_[pid];
        lastErrorType_ = type;
        lastPid_ = pid;
        lastPreviousCc_ = previous;
        lastCurrentCc_ = current;
        lastExpectedCc_ = expected;
        lastAfc_ = afc;
        lastHasPayload_ = hasPayload;
        lastPusi_ = pusi;
        lastDiscontinuity_ = discontinuity;
    }

    void inspectPacket(const uint8_t* packet) {
        if (!packet || packet[0] != 0x47) {
            ++badSync_;
            return;
        }
        ++packets_;

        const bool tei = (packet[1] & 0x80U) != 0;
        if (tei) ++teiPackets_;
        const bool pusi = (packet[1] & 0x40U) != 0;
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1FU) << 8) | packet[2]);
        if (pid == 0x1FFF) return;

        const uint8_t afc = static_cast<uint8_t>((packet[3] >> 4) & 0x03U);
        const uint8_t cc = static_cast<uint8_t>(packet[3] & 0x0FU);
        if (afc == 0) {
            ++badAfc_;
            valid_[pid] = false;
            hashValid_[pid] = false;
            return;
        }

        bool discontinuity = false;
        if ((afc == 2 || afc == 3) && packet[4] > 0 && packet[4] <= 183) {
            discontinuity = (packet[5] & 0x80U) != 0;
        }
        if (discontinuity) {
            ++discontinuities_;
            valid_[pid] = false;
            hashValid_[pid] = false;
        }

        const bool hasPayload = afc == 1 || afc == 3;
        if (!hasPayload) {
            ++adaptationOnlyPackets_;
            if (valid_[pid] && cc != lastCc_[pid]) {
                ++adaptationOnlyCcErrors_;
                recordCcError(pid, "adaptation_only_cc", lastCc_[pid], cc,
                              lastCc_[pid], afc, false, pusi, discontinuity);
            }
            return;
        }

        const uint64_t hash = packetHash(packet);
        if (!valid_[pid]) {
            lastCc_[pid] = cc;
            valid_[pid] = true;
            lastPayloadHash_[pid] = hash;
            hashValid_[pid] = true;
            return;
        }

        const uint8_t previous = lastCc_[pid];
        const uint8_t expected = static_cast<uint8_t>((previous + 1) & 0x0F);
        if (cc == previous && hashValid_[pid] && lastPayloadHash_[pid] == hash) {
            ++exactDuplicates_;
            ++pidDuplicates_[pid];
            return;
        }

        if (cc != expected) {
            recordCcError(pid, "payload_cc", previous, cc, expected,
                          afc, true, pusi, discontinuity);
        }

        lastCc_[pid] = cc;
        valid_[pid] = true;
        lastPayloadHash_[pid] = hash;
        hashValid_[pid] = true;
    }

    std::string topPidDeltas() {
        std::array<std::pair<uint64_t, uint16_t>, 8> top{};
        for (uint16_t pid = 0; pid < kPidCount; ++pid) {
            const uint64_t current = pidErrors_[pid];
            const uint64_t delta = current - pidErrorsLogged_[pid];
            pidErrorsLogged_[pid] = current;
            if (delta == 0) continue;
            std::pair<uint64_t, uint16_t> candidate{delta, pid};
            for (auto& slot : top) {
                if (candidate.first > slot.first) std::swap(candidate, slot);
            }
        }
        std::ostringstream out;
        bool first = true;
        for (const auto& [count, pid] : top) {
            if (!count) continue;
            if (!first) out << ',';
            out << pid << ':' << count;
            first = false;
        }
        return first ? "none" : out.str();
    }

    void maybeLog() {
        const uint64_t now = nowMs();
        if (lastLogMs_ != 0 && now >= lastLogMs_ && now - lastLogMs_ < 1000) return;
        lastLogMs_ = now;

        const uint64_t errorDelta = ccErrors_ - loggedCcErrors_;
        const uint64_t duplicateDelta = exactDuplicates_ - loggedDuplicates_;
        const uint64_t teiDelta = teiPackets_ - loggedTei_;
        const uint64_t badAfcDelta = badAfc_ - loggedBadAfc_;
        const std::string pidDeltas = topPidDeltas();
        loggedCcErrors_ = ccErrors_;
        loggedDuplicates_ = exactDuplicates_;
        loggedTei_ = teiPackets_;
        loggedBadAfc_ = badAfc_;

        std::cerr << "CC STAGE DIAG: stream=" << streamId_
                  << " stage=" << stage_
                  << " calls=" << calls_
                  << " buffer_size=" << lastBufferSize_
                  << " packets=" << packets_
                  << " cc_errors=" << ccErrors_
                  << " cc_delta=" << errorDelta
                  << " pid_errors_delta=" << pidDeltas
                  << " exact_duplicates=" << exactDuplicates_
                  << " duplicate_delta=" << duplicateDelta
                  << " adaptation_only=" << adaptationOnlyPackets_
                  << " adaptation_cc_errors=" << adaptationOnlyCcErrors_
                  << " discontinuities=" << discontinuities_
                  << " tei=" << teiPackets_
                  << " tei_delta=" << teiDelta
                  << " bad_afc=" << badAfc_
                  << " bad_afc_delta=" << badAfcDelta
                  << " unaligned_buffers=" << unalignedBuffers_
                  << " alignment_misses=" << alignmentMisses_
                  << " resync_bytes=" << resyncBytes_;
        if (!lastErrorType_.empty()) {
            std::cerr << " last_type=" << lastErrorType_
                      << " last_pid=" << lastPid_
                      << " prev_cc=" << static_cast<unsigned>(lastPreviousCc_)
                      << " cc=" << static_cast<unsigned>(lastCurrentCc_)
                      << " expected=" << static_cast<unsigned>(lastExpectedCc_)
                      << " afc=" << static_cast<unsigned>(lastAfc_)
                      << " payload=" << (lastHasPayload_ ? 1 : 0)
                      << " pusi=" << (lastPusi_ ? 1 : 0)
                      << " discontinuity=" << (lastDiscontinuity_ ? 1 : 0);
        }
        std::cerr << std::endl;
    }

    std::string streamId_;
    std::string stage_;
    std::vector<uint8_t> remainder_;
    std::array<uint8_t, kPidCount> lastCc_{};
    std::array<bool, kPidCount> valid_{};
    std::array<uint64_t, kPidCount> lastPayloadHash_{};
    std::array<bool, kPidCount> hashValid_{};
    std::array<uint64_t, kPidCount> pidErrors_{};
    std::array<uint64_t, kPidCount> pidErrorsLogged_{};
    std::array<uint64_t, kPidCount> pidDuplicates_{};

    uint64_t calls_ = 0;
    uint64_t packets_ = 0;
    uint64_t ccErrors_ = 0;
    uint64_t exactDuplicates_ = 0;
    uint64_t adaptationOnlyPackets_ = 0;
    uint64_t adaptationOnlyCcErrors_ = 0;
    uint64_t discontinuities_ = 0;
    uint64_t teiPackets_ = 0;
    uint64_t badAfc_ = 0;
    uint64_t badSync_ = 0;
    uint64_t unalignedBuffers_ = 0;
    uint64_t alignmentMisses_ = 0;
    uint64_t resyncBytes_ = 0;
    uint64_t lastBufferSize_ = 0;

    uint64_t lastLogMs_ = 0;
    uint64_t loggedCcErrors_ = 0;
    uint64_t loggedDuplicates_ = 0;
    uint64_t loggedTei_ = 0;
    uint64_t loggedBadAfc_ = 0;

    std::string lastErrorType_;
    uint16_t lastPid_ = 0;
    uint8_t lastPreviousCc_ = 0;
    uint8_t lastCurrentCc_ = 0;
    uint8_t lastExpectedCc_ = 0;
    uint8_t lastAfc_ = 0;
    bool lastHasPayload_ = false;
    bool lastPusi_ = false;
    bool lastDiscontinuity_ = false;
};
