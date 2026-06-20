#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

enum class SmartTransferExpiration
{
    FifteenMinutes = 15,
    ThirtyMinutes = 30,
    OneHour = 60,
    Manual = 0
};

enum class SmartTransferHostStatus
{
    Idle,
    Preparing,
    Hosting,
    WaitingForApproval,
    Sending,
    Complete,
    Failed,
    Cancelled
};

enum class SmartTransferClientStatus
{
    Idle,
    Connecting,
    WaitingForApproval,
    Connected,
    Downloading,
    Complete,
    Failed,
    Cancelled
};

enum class SmartTransferWebRtcStatus
{
    Disabled,
    DependencyMissing,
    NotNeededYet,
    ReadyForFallback,
    WaitingForReceiverResponse,
    Pairing,
    Connected,
    Failed
};

struct SmartTransferFile
{
    std::wstring id;
    std::filesystem::path path;
    std::wstring name;
    std::wstring extension;
    unsigned long long size = 0;
};

struct SmartTransferManifestFile
{
    std::wstring id;
    std::wstring name;
    std::wstring extension;
    unsigned long long size = 0;
    bool selected = true;
    std::wstring status;
};

struct SmartTransferManifest
{
    int version = 1;
    std::wstring transferName;
    long long expiresAt = 0;
    std::vector<SmartTransferManifestFile> files;
};

struct SmartTransferSendOptions
{
    SmartTransferExpiration expiration = SmartTransferExpiration::ThirtyMinutes;
    bool requireApproval = true;
    bool stopAfterFirstCompletedDownload = true;
    bool allowMultipleReceivers = false;
    bool tryDirectHost = false;
    bool enableWebRtcFallback = true;
    bool showWebRtcDiagnostics = false;
    std::vector<std::wstring> stunServers { L"stun:stun.l.google.com:19302" };
    std::wstring transferName;
};

struct SmartTransferInvite
{
    int version = 1;
    std::wstring sessionId;
    std::wstring token;
    long long expiresAt = 0;
    std::wstring lanUrl;
    std::wstring directUrl;
    std::wstring activeUrl;
    std::wstring capabilities;
};

struct SmartTransferHostSnapshot
{
    SmartTransferHostStatus status = SmartTransferHostStatus::Idle;
    std::wstring sessionId;
    std::wstring token;
    std::wstring transferCode;
    std::wstring localUrl;
    std::wstring directUrl;
    std::wstring publicIp;
    std::wstring message;
    std::wstring directHostMessage;
    std::wstring webRtcMessage;
    std::wstring webRtcDiagnostics;
    std::wstring senderPairingCode;
    std::wstring receiverAddress;
    std::wstring currentFile;
    long long expiresAt = 0;
    unsigned short port = 0;
    unsigned long long bytesSent = 0;
    unsigned long long totalBytes = 0;
    int receiverCount = 0;
    bool approvalPending = false;
    bool approved = false;
    bool denied = false;
    bool directHostRequested = false;
    bool directHostAvailable = false;
    bool portMappingActive = false;
    bool webRtcFallbackEnabled = true;
    bool webRtcDependencyAvailable = false;
    SmartTransferWebRtcStatus webRtcStatus = SmartTransferWebRtcStatus::NotNeededYet;
};

struct SmartTransferConnectResult
{
    bool success = false;
    bool waitingForApproval = false;
    bool webRtcFallbackOffered = false;
    bool webRtcDependencyMissing = false;
    SmartTransferManifest manifest;
    SmartTransferInvite invite;
    std::wstring message;
    std::wstring webRtcMessage;
};

struct SmartTransferDownloadProgress
{
    SmartTransferClientStatus status = SmartTransferClientStatus::Idle;
    std::wstring currentFile;
    std::wstring message;
    unsigned long long bytesDownloaded = 0;
    unsigned long long totalBytes = 0;
};

struct SmartTransferWebRtcSnapshot
{
    SmartTransferWebRtcStatus status = SmartTransferWebRtcStatus::NotNeededYet;
    SmartTransferManifest manifest;
    std::wstring senderPairingCode;
    std::wstring receiverResponseCode;
    std::wstring message;
    std::wstring diagnostics;
    bool pairingCodeReady = false;
    bool responseCodeReady = false;
    bool connected = false;
    bool manifestReady = false;
};

class TransferSecurityService
{
public:
    static std::wstring GenerateHexToken(size_t bytes);
    static std::wstring CreateFileId(size_t index);
    static std::wstring SafeFileName(const std::wstring& name);
    static std::filesystem::path AutoRenamePath(const std::filesystem::path& requestedPath);
    static bool IsExpired(long long expiresAt);
};

class TransferCodeService
{
public:
    static std::wstring Encode(const SmartTransferInvite& invite);
    static bool Decode(const std::wstring& code, SmartTransferInvite& invite, std::wstring& errorMessage);
};

class WebRtcSenderSession;
class WebRtcReceiverSession;

class TransferHostServer
{
public:
    TransferHostServer();
    ~TransferHostServer();

    bool Start(
        const std::vector<SmartTransferFile>& files,
        const SmartTransferSendOptions& options,
        std::wstring& errorMessage);
    void Stop();
    void AllowPendingReceiver();
    void DenyPendingReceiver();
    SmartTransferHostSnapshot Snapshot() const;
    bool CreateWebRtcSenderPairingCode(std::wstring& pairingCode, std::wstring& errorMessage);
    bool ApplyWebRtcReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage);
    SmartTransferWebRtcSnapshot WebRtcSnapshot() const;

private:
    void ServerLoop();
    void HandleClient(uintptr_t clientSocket, std::wstring remoteAddress);
    void CloseListenSocket();
    std::string BuildManifestJson() const;
    std::optional<SmartTransferFile> FileById(const std::wstring& id) const;
    bool TokenAccepted(const std::wstring& token) const;
    bool ApprovalAccepted(const std::wstring& remoteAddress, const std::wstring& requestLabel);
    bool PrepareDirectHost(
        SmartTransferInvite& invite,
        const std::wstring& localIp,
        unsigned short port,
        std::wstring& directHostMessage);
    bool QueryPublicIpv4(std::wstring& publicIp, std::wstring& errorMessage) const;
    bool AddUpnpPortMapping(unsigned short externalPort, unsigned short internalPort, const std::wstring& localIp, std::wstring& errorMessage);
    void RemovePortMapping();
    bool TestPublicReachability(const std::wstring& publicUrl, const std::wstring& token, std::wstring& errorMessage) const;

    mutable std::mutex mutex_;
    mutable std::mutex mappingMutex_;
    std::vector<SmartTransferFile> files_;
    std::set<std::wstring> servedFileIds_;
    SmartTransferSendOptions options_;
    SmartTransferHostSnapshot snapshot_;
    std::thread serverThread_;
    std::atomic_bool stopRequested_ = true;
    uintptr_t listenSocket_ = static_cast<uintptr_t>(~0ull);
    mutable std::mutex webRtcSenderMutex_;
    std::shared_ptr<WebRtcSenderSession> webRtcSender_;
    bool winsockStarted_ = false;
    bool portMappingActive_ = false;
    unsigned short mappedExternalPort_ = 0;
    std::wstring mappedProtocol_;
};

class TransferClient
{
public:
    SmartTransferConnectResult Connect(const std::wstring& code) const;
    bool DownloadSelected(
        const SmartTransferInvite& invite,
        const SmartTransferManifest& manifest,
        const std::filesystem::path& saveFolder,
        const std::atomic_bool& cancelRequested,
        const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
        std::wstring& errorMessage) const;
    static bool ParseManifest(const std::string& json, SmartTransferManifest& manifest, std::wstring& errorMessage);

private:
    static bool HttpGet(
        const std::wstring& url,
        std::string& body,
        unsigned long& statusCode,
        std::wstring& errorMessage);
    static bool HttpDownloadToFile(
        const std::wstring& url,
        const std::filesystem::path& destination,
        const std::atomic_bool& cancelRequested,
        const std::function<void(unsigned long long)>& progressCallback,
        unsigned long& statusCode,
        std::wstring& errorMessage);
};

class PortMappingService
{
public:
    static std::wstring StatusMessage();
};

class WebRtcTransport
{
public:
    static bool IsAvailable();
    static std::wstring DependencyMessage();
    static std::wstring StatusMessage();
};

class WebRtcOfferAnswerService
{
public:
    static std::wstring DependencyMessage();
    static bool CanCreatePairingCodes();
};

class WebRtcSenderSession
{
public:
    WebRtcSenderSession();
    ~WebRtcSenderSession();
    WebRtcSenderSession(const WebRtcSenderSession&) = delete;
    WebRtcSenderSession& operator=(const WebRtcSenderSession&) = delete;

    static bool IsAvailable();
    bool Start(
        const std::vector<SmartTransferFile>& files,
        const SmartTransferSendOptions& options,
        const SmartTransferInvite& invite,
        const std::function<bool(const std::wstring&, const std::wstring&)>& approvalCallback,
        std::wstring& pairingCode,
        std::wstring& errorMessage);
    bool ApplyReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage);
    void SetApproval(bool approved, bool denied);
    void Stop();
    SmartTransferWebRtcSnapshot Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WebRtcReceiverSession
{
public:
    WebRtcReceiverSession();
    ~WebRtcReceiverSession();
    WebRtcReceiverSession(const WebRtcReceiverSession&) = delete;
    WebRtcReceiverSession& operator=(const WebRtcReceiverSession&) = delete;

    static bool IsAvailable();
    bool CreateResponse(
        const SmartTransferInvite& invite,
        const SmartTransferSendOptions& options,
        const std::wstring& senderPairingCode,
        std::wstring& responseCode,
        std::wstring& errorMessage);
    bool DownloadSelected(
        const SmartTransferManifest& manifest,
        const std::filesystem::path& saveFolder,
        const std::atomic_bool& cancelRequested,
        const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
        std::wstring& errorMessage);
    void Stop();
    SmartTransferWebRtcSnapshot Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WebRtcChunkSender
{
public:
    static constexpr unsigned long long PreferredChunkSize = 64ull * 1024ull;
};

class WebRtcChunkReceiver
{
public:
    static constexpr unsigned long long PreferredChunkSize = WebRtcChunkSender::PreferredChunkSize;
};

class SmartFileTransferService
{
public:
    ~SmartFileTransferService();

    std::optional<SmartTransferFile> CreateTransferFile(const std::filesystem::path& path, size_t index, std::wstring& errorMessage) const;
    bool StartHosting(
        const std::vector<SmartTransferFile>& files,
        const SmartTransferSendOptions& options,
        std::wstring& errorMessage);
    void StopHosting();
    void AllowPendingReceiver();
    void DenyPendingReceiver();
    SmartTransferHostSnapshot HostSnapshot() const;
    SmartTransferConnectResult Connect(const std::wstring& code) const;
    bool CreateWebRtcSenderPairingCode(std::wstring& pairingCode, std::wstring& errorMessage);
    bool ApplyWebRtcReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage);
    bool CreateWebRtcReceiverResponse(
        const SmartTransferInvite& invite,
        const std::vector<std::wstring>& stunServers,
        const std::wstring& senderPairingCode,
        std::wstring& responseCode,
        std::wstring& errorMessage);
    SmartTransferWebRtcSnapshot WebRtcSnapshot() const;
    bool DownloadSelected(
        const SmartTransferInvite& invite,
        const SmartTransferManifest& manifest,
        const std::filesystem::path& saveFolder,
        const std::atomic_bool& cancelRequested,
        const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
        std::wstring& errorMessage) const;

private:
    TransferHostServer hostServer_;
    TransferClient client_;
    mutable std::mutex webRtcReceiverMutex_;
    std::shared_ptr<WebRtcReceiverSession> webRtcReceiver_;
};
