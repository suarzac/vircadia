//
//  DomainServerAcmeClient.cpp
//  domain-server/src
//
//  Created by Nshan G. on 2021-11-15.
//  Copyright 2021 Vircadia contributors.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DomainServerAcmeClient.h"

#include <HTTPManager.h>
#include <HTTPConnection.h>
#include <PathUtils.h>
#include <acme/acme-lw.hpp>
#include <SSLCommon.h>

#include <QDir>

#include <memory>
#include <array>
#include <set>

#include "DomainServerSettingsManager.h"


Q_LOGGING_CATEGORY(acme_client, "vircadia.acme_client")

using namespace std::literals;
using std::chrono::system_clock;

std::string readAll(const QString& path)
{
    QFile file(path);
    if(file.open(QFile::ReadOnly)) {
        return file.readAll().toStdString();
    }
    return std::string();
}

bool writeAll(const std::string& data, const QString& path)
{
    QFile file(path);
    return file.open(QFile::WriteOnly) &&
        file.write(QByteArray::fromStdString(data)) == qint64(data.size());
}


class AcmeHttpChallengeServer : public AcmeChallengeHandler, public HTTPRequestHandler {
    struct Challenge {
        QUrl url;
        QByteArray content;
    };

public:
    AcmeHttpChallengeServer() :
        manager(QHostAddress::AnyIPv4, 80, "", this)
    {}

    void addChallenge(const std::string&, const std::string& location, const std::string& content) override {
        challenges.push_back({
            QString::fromStdString(location),
            QByteArray::fromStdString(content)
        });
    }

    std::chrono::milliseconds selfCheckDuration() override { return 1s; }
    std::chrono::milliseconds selfCheckInterval() override { return 250ms; }

    bool handleHTTPRequest(HTTPConnection* connection, const QUrl& url, bool skipSubHandler = false) override {
        auto challenge = std::find_if(challenges.begin(), challenges.end(), [&url](auto x) { return x.url == url; });
        if(challenge != challenges.end()) {
            connection->respond(HTTPConnection::StatusCode200, challenge->content, "application/octet-stream");
        } else {
            auto chstr = ""s;
            for(auto&& ch : challenges) {
                chstr += ch.url.toString().toStdString();
                chstr += '\n';
            }
            connection->respond(HTTPConnection::StatusCode404, ("Resource not found. Url is "s + url.toString().toStdString() + " but expected any of\n"s + chstr).c_str());
        }
        return true;
    }

private:
    HTTPManager manager;
    std::vector<Challenge> challenges;
};

class AcmeHttpChallengeFiles : public AcmeChallengeHandler {

public:
    AcmeHttpChallengeFiles(std::map<std::string, std::string> dirs) :
        dirs(std::move(dirs))
    {}

    ~AcmeHttpChallengeFiles() override {
        std::set<QString> challengeDirs;

        for(auto&& challengePath : challengePaths) {
            challengeDirs.insert(QFileInfo(challengePath).path());
            if(!QFile(challengePath).remove()) {
                qCWarning(acme_client) << "Failed to remove challenge file:" << challengePath;
            }
        }

        for(auto&& challengeDir : challengeDirs) {
            if(!QDir(challengeDir).rmdir(".")) {
                qCWarning(acme_client) << "Failed to remove challenge directory:" << challengeDir;
            }
        }
    }

    std::chrono::milliseconds selfCheckDuration() override { return 2s; }
    std::chrono::milliseconds selfCheckInterval() override { return 250ms; }

    void addChallenge(const std::string& domain, const std::string& location, const std::string& content) override {
        QString challengePath = (dirs[domain] + location).c_str();
        QDir challengeDir = QFileInfo(challengePath).path();
        if(challengeDir.mkpath(".") && writeAll(content, challengePath)) {
            challengePaths.push_back(challengePath);
        } else {
            qCCritical(acme_client) << "Failed to write challenge file:" << challengePath;
        }
    }

private:
    std::map<std::string, std::string> dirs;
    std::vector<QString> challengePaths;
};

class AcmeHttpChallengeManual : public AcmeChallengeHandler {
public:
    void addChallenge(const std::string& domain, const std::string& location, const std::string& content) override {
        qCDebug(acme_client) << "Please manually complete this http challenge:\n"
            << "Domain:" << domain.c_str() << '\n'
            << "Location:" << location.c_str() << '\n'
            << "Content:" << content.c_str() << '\n';
    }

    std::chrono::milliseconds selfCheckDuration() override { return 120s; }
    std::chrono::milliseconds selfCheckInterval() override { return 1s; }

};

struct ChallengeHandlerParams {
    std::string typeId;
    std::map<std::string, std::string> domainDirs;
};

std::unique_ptr<AcmeChallengeHandler> makeChallengeHandler(ChallengeHandlerParams params) {

    if(params.typeId == "server") {
        return std::make_unique<AcmeHttpChallengeServer>();
    } else if(params.typeId == "files") {
        return std::make_unique<AcmeHttpChallengeFiles>(params.domainDirs);
    } else if(params.typeId == "manual") {
        return std::make_unique<AcmeHttpChallengeManual>();
    } else {
        throw std::logic_error("Invalid ACME HTTP challenge handler type id: " + params.typeId);
    }
};

template <typename Callback>
class ChallengeSelfCheck :
    public std::enable_shared_from_this< ChallengeSelfCheck<Callback> >
{
    public:
    ChallengeSelfCheck(Callback callback, std::vector<std::string> urls) :
        callback(std::move(callback)),
        urls(std::move(urls))
    {}

    void start(std::chrono::milliseconds duration, std::chrono::milliseconds interval) {
        for(auto&& url : urls) {
            acme_lw::waitForGet(shared_callback{this->shared_from_this()},
                std::move(url), duration, interval);
        }
    }

    ~ChallengeSelfCheck() {
        callback();
    }

    void operator()(acme_lw::Response) const {}

    void operator()(acme_lw::AcmeException error) const {
        qCWarning(acme_client) << "Challenge self-check failed: " << error.what() << '\n';
    }

    private:
    struct shared_callback {
        std::shared_ptr<ChallengeSelfCheck> ptr;
        template <typename... Args>
        void operator()(Args&&... args) {
            ptr->operator()(std::forward<Args>(args)...);
        }
    };

    Callback callback;
    std::vector<std::string> urls;

};

template <typename Callback>
auto challengeSelfCheck(Callback callback, std::vector<std::string> urls) {
    return std::make_shared<ChallengeSelfCheck<Callback>>(
        std::move(callback), std::move(urls));
}

bool createAccountKey(QFile& file) {
    if (file.open(QFile::WriteOnly)) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.write(QByteArray::fromStdString(acme_lw::toPemString(acme_lw::makePrivateKey())));
        file.close();
        return true;
    }
    return false;
}

acme_lw::Certificate readCertificate(const CertificatePaths& files) {
    return {
        readAll(files.cert),
        readAll(files.key),
    };
}

bool writeCertificate(acme_lw::Certificate cert, const CertificatePaths& files) {
    return writeAll(cert.fullchain, files.cert) &&
        writeAll(cert.privkey, files.key);
}

QString DomainServerAcmeClient::getAccountKeyPath(DomainServerSettingsManager& settings) {
    return settings.valueOrDefaultValueForKeyPath("acme.account_key_path").toString();
}

CertificatePaths DomainServerAcmeClient::getCertificatePaths(DomainServerSettingsManager& settings) {
    auto certDirStr = settings.valueOrDefaultValueForKeyPath("acme.certificate_directory").toString();
    QDir certDir(certDirStr != ""
        ? QDir(certDirStr)
        : PathUtils::getAppLocalDataPath()
    );

    CertificatePaths paths {
        settings.valueOrDefaultValueForKeyPath("acme.certificate_filename").toString(),
        settings.valueOrDefaultValueForKeyPath("acme.certificate_key_filename").toString(),
        settings.valueOrDefaultValueForKeyPath("acme.certificate_authority_filename").toString()
    };

    paths.cert = certDir.filePath(paths.cert);
    paths.key = certDir.filePath(paths.key);
    paths.trustedAuthorities = certDir.filePath(paths.trustedAuthorities);
    return paths;
}

DomainServerAcmeClient::DomainServerAcmeClient(DomainServerSettingsManager& settings) :
    renewalTimer(),
    challengeHandler(nullptr),
    selfCheckUrls(),
    settings(settings)
{
    renewalTimer.setSingleShot(true);
    connect(&renewalTimer, &QTimer::timeout, this, [this](){ init(); });

    connect(&updateCheckTimer, &QTimer::timeout, this, [this](){
        auto paths = getCertificatePaths(this->settings);
        if(QFile::exists(paths.cert) && QFile::exists(paths.key)) {
            auto cert = readCertificate(paths);
            if(!cert.fullchain.empty() && !cert.privkey.empty()) {
                auto newExpiry = cert.getExpiryOrError();
                if(newExpiry.success) {
                    if(this->expiry < newExpiry.value) {
                        emit certificateUpdated(paths);
                        this->expiry = newExpiry.value;
                    }
                }
            }
        }
    });

    updateCheckTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(24h).count());

    init();
}

void setError(nlohmann::json& json, std::string type)
{
    json["status"] = "error";
    json["error"] = {
        {"type", type}
    };
}

void setError(nlohmann::json& json, std::string type, nlohmann::json data)
{
    setError(json, type);
    json["error"]["data"] = data;
}

void DomainServerAcmeClient::init() {

    status.clear();
    status = nlohmann::json({
        {"directory", {
            {"status", "unknown"},
        }},
        {"account", {
            {"status", "unknown"},
        }},
        {"certificate", {
            {"status", "unknown"},
        }}
    });

    bool enabled = settings.valueOrDefaultValueForKeyPath("acme.enable_client").toBool();
    if(!enabled) {
        return;
    }

    auto certPaths = DomainServerAcmeClient::getCertificatePaths(settings);
    std::array<QString,2> pathsByExistence { certPaths.cert, certPaths.key };
    auto notExisitng = std::stable_partition(pathsByExistence.begin(), pathsByExistence.end(),
        [](auto x){ return QFile::exists(x); });
    if(notExisitng == pathsByExistence.end()) {
        // all files exist, order unchanged
        checkExpiry(certPaths);
    } else if(notExisitng == pathsByExistence.begin()) {
        // none of the files exist, order unchanged
        generateCertificate(certPaths);
    } else {
        setError(status["certificate"], "missing", {
            {"missing", notExisitng->toStdString()},
            {"present", pathsByExistence.begin()->toStdString()}
        });
        // one file exist while the other doesn't, ordered existing first
        qCCritical(acme_client) << "SSL certificate missing file:\n" << *notExisitng;
        qCCritical(acme_client) << "Either provide it, or remove the other file to generate a new certificate:\n" << *pathsByExistence.begin();
        return;
    }
}

system_clock::duration remainingTime(system_clock::time_point expiryTime) {
    return (expiryTime - system_clock::now()) * 2 / 3;
}

std::chrono::seconds secondsSinceEpoch(system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch());
}

QDateTime dateTimeFrom(system_clock::time_point time) {
    QDateTime scheduleTime;
    scheduleTime.setSecsSinceEpoch(secondsSinceEpoch(time).count());
    return scheduleTime;
}

template<typename Callback>
struct CertificateCallback{
    nlohmann::json& status;
    std::unique_ptr<AcmeChallengeHandler>& challengeHandler;
    CertificatePaths certPaths;
    Callback next;

    void operator()(acme_lw::AcmeClient client, acme_lw::Certificate cert) const {
        challengeHandler = nullptr;
        qCDebug(acme_client) << "Certificate retrieved\n"
            << "Expires on:" << dateTimeFrom(cert.getExpiry()) << '\n'
        ;
        bool success = writeCertificate(cert, certPaths);
        if(!success) {
            const char* message = "Failed to write certificate files.";
            setError(status["certificate"], "write", {
                {"message", message}
            });
            qCCritical(acme_client) << message << '\n'
                << certPaths.cert << '\n'
                << certPaths.key << '\n';
        }
        next(std::move(cert), std::move(certPaths), success);
    }
    void operator()(acme_lw::AcmeClient client, acme_lw::AcmeException error) const {
        setError(status["certificate"], "acme", {
            {"message", error.what()}
        });
        challengeHandler = nullptr;
        qCCritical(acme_client) << error.what() << '\n';
        qCDebug(acme_client) << status.dump(1).c_str() << '\n';
        next(acme_lw::Certificate(), std::move(certPaths), false);
    }
};

template<typename Callback>
CertificateCallback<Callback> certificateCallback(
    nlohmann::json& status,
    std::unique_ptr<AcmeChallengeHandler>& challengeHandler,
    CertificatePaths certPaths,
    Callback next
) {
    return {
        status,
        challengeHandler,
        std::move(certPaths),
        std::move(next)
    };
}

template <typename Callback>
struct OrderCallback{
    nlohmann::json& status;
    std::unique_ptr<AcmeChallengeHandler>& challengeHandler;
    std::vector<std::string>& selfCheckUrls;
    CertificatePaths certPaths;
    Callback next;

    void operator()(acme_lw::AcmeClient client, std::vector<std::string> challenges, std::vector<std::string> domains, std::string finalUrl, std::string orderUrl) const {
        qCDebug(acme_client) << "Ordered certificate\n"
            << "Order URL:" << orderUrl.c_str() << '\n'
            << "Finalize URL:" << finalUrl.c_str() << '\n'
            << "Number of domains:" << domains.size() << '\n'
            << "Number of challenges:" << challenges.size() << '\n'
        ;
        challengeSelfCheck([client = std::move(client), orderUrl, finalUrl, challenges, domains,
                &status = status, &challengeHandler = challengeHandler, certPaths = std::move(certPaths), next = std::move(next)]() mutable {
            retrieveCertificate(certificateCallback(status, challengeHandler, std::move(certPaths), std::move(next)),
                std::move(client), std::move(domains), std::move(challenges),
                std::move(orderUrl), std::move(finalUrl)
            );
        }, std::move(selfCheckUrls))->start(challengeHandler->selfCheckDuration(), challengeHandler->selfCheckInterval());
        selfCheckUrls.clear();
    }
    void operator()(acme_lw::AcmeClient client, acme_lw::AcmeException error) const {
        setError(status["certificate"], "acme", {
            {"message", error.what()}
        });
        qCCritical(acme_client) << error.what() << '\n';
        qCDebug(acme_client) << status.dump(1).c_str() << '\n';
        selfCheckUrls.clear();
        next(acme_lw::Certificate(), std::move(certPaths), false);
    }
};

template<typename Callback>
OrderCallback<Callback> orderCallback(
    nlohmann::json& status,
    std::unique_ptr<AcmeChallengeHandler>& challengeHandler,
    std::vector<std::string>& selfCheckUrls,
    CertificatePaths certPaths,
    Callback next
) {
    return {
        status,
        challengeHandler,
        selfCheckUrls,
        std::move(certPaths),
        std::move(next)
    };
}

template <typename Callback>
struct AccountCallback{
    nlohmann::json& status;
    std::unique_ptr<AcmeChallengeHandler>& challengeHandler;
    std::vector<std::string>& selfCheckUrls;
    CertificatePaths certPaths;
    std::vector<std::string> domains;
    ChallengeHandlerParams challengeHandlerParams;
    Callback next;

    void operator()(acme_lw::AcmeClient client) const {
        status["account"]["status"] = "ok";
        status["certificate"]["status"] = "pending";
        acme_lw::orderCertificate(orderCallback(status, challengeHandler, selfCheckUrls, std::move(certPaths), std::move(next)), [
                &challengeHandler = challengeHandler,
                &selfCheckUrls = selfCheckUrls,
                challengeHandlerParams = std::move(challengeHandlerParams)
        ](auto domain, auto location, auto keyAuth){
                qCDebug(acme_client) << "Got challenge:\n"
                    << "Domain:" << domain.c_str() << '\n'
                    << "Location:" << location.c_str() << '\n'
                    << "Key Authorization:" << keyAuth.c_str() << '\n'
                ;
                if(!challengeHandler) {
                    challengeHandler = makeChallengeHandler(challengeHandlerParams);
                }
                challengeHandler->addChallenge(domain, location, keyAuth);
                selfCheckUrls.push_back("http://"s + domain + location);
        }, std::move(client), std::move(domains));
    }

    void operator()(acme_lw::AcmeClient client, acme_lw::AcmeException error) const {
        setError(status["account"], "acme", {
            {"message", error.what()}
        });
        qCCritical(acme_client) << error.what() << '\n';
        qCDebug(acme_client) << status.dump(1).c_str() << '\n';
        selfCheckUrls.clear();
        next(acme_lw::Certificate(), std::move(certPaths), false);
    }

    void operator()(acme_lw::AcmeException error) const {
        setError(status["directory"], "acme", {
            {"message", error.what()}
        });
        qCCritical(acme_client) << error.what() << '\n';
        qCDebug(acme_client) << status.dump(1).c_str() << '\n';
        selfCheckUrls.clear();
        next(acme_lw::Certificate(), std::move(certPaths), false);
    }
};

template<typename Callback>
AccountCallback<Callback> accountCallback(
    nlohmann::json& status,
    std::unique_ptr<AcmeChallengeHandler>& challengeHandler,
    std::vector<std::string>& selfCheckUrls,
    CertificatePaths certPaths,
    std::vector<std::string> domains,
    ChallengeHandlerParams challengeHandlerParams,
    Callback next
) {
    return {
        status,
        challengeHandler,
        selfCheckUrls,
        std::move(certPaths),
        std::move(domains),
        std::move(challengeHandlerParams),
        std::move(next)
    };
}

void DomainServerAcmeClient::generateCertificate(const CertificatePaths& certPaths) {

    QString accountKeyPath = getAccountKeyPath(settings);
    if(accountKeyPath == "") {
        accountKeyPath = QDir(PathUtils::getAppLocalDataPath()).filePath("acme_account_key.pem");
    }
    QFile accountKeyFile(accountKeyPath);
    if(!accountKeyFile.exists()) {
        if(!createAccountKey(accountKeyFile)) {
            setError(status["account"], "key-write");
            qCCritical(acme_client) << "Failed to create account key file " << accountKeyFile.fileName();
            return;
        }
    }

    std::string accountKey = "";
    if(accountKeyFile.open(QFile::ReadOnly))
    {
        accountKey = accountKeyFile.readAll().toStdString();
        accountKeyFile.close();
    } else {
        setError(status["account"], "key-read");
        qCCritical(acme_client) << "Failed to read account key file " << accountKeyFile.fileName();
        return;
    }


    std::vector<std::string> domains;

    std::map<std::string, std::string> domainDirs;
    auto domainList = settings.valueOrDefaultValueForKeyPath("acme.certificate_domains").toList();
    for(auto&& var : domainList) {
        auto map = var.toMap();
        domains.push_back(QUrl::toAce(map["domain"].toString()).toStdString());
        auto domainDir = map["directory"].toString().toStdString();
        domainDirs[domains.back()] = domainDir != "" ? domainDir : ".";
    }

    ChallengeHandlerParams challengeHandlerParams {
        settings.valueOrDefaultValueForKeyPath("acme.challenge_handler_type").toString().toStdString(),
        std::move(domainDirs)
    };

    auto directoryUrl = settings.valueOrDefaultValueForKeyPath("acme.directory_endpoint").toString().toStdString();
    auto eabKid = settings.valueOrDefaultValueForKeyPath("acme.eab_kid").toString().toStdString();
    auto eabHmac = settings.valueOrDefaultValueForKeyPath("acme.eab_mac").toString().toStdString();

    selfCheckUrls.clear(); // just in case

    status["directory"]["status"] = "pending";
    acme_lw::init(acme_lw::forwardAcmeError([this, domains = std::move(domains), challengeHandlerParams = std::move(challengeHandlerParams)](auto next, auto client){
        status["directory"]["status"] = "ok";
        status["account"]["status"] = "pending";
        acme_lw::createAccount(std::move(next), std::move(client));
    }, accountCallback(status, challengeHandler, selfCheckUrls, certPaths, std::move(domains), std::move(challengeHandlerParams),
        [this](auto cert, auto certPaths, bool success){
            if(success) {
                emit certificateUpdated(certPaths);
                handleRenewal(cert.getExpiry(), certPaths);
            } else {
                scheduleRenewalIn(24h);
            }
        }
    )), accountKey, directoryUrl, eabKid, eabHmac);

}

void DomainServerAcmeClient::checkExpiry(const CertificatePaths& certPaths) {
    auto cert = readCertificate(certPaths);
    if(cert.fullchain.empty() || cert.privkey.empty()) {
        const char* message = "Failed to read certificate files.";
        setError(status["certificate"], "invalid", {
            {"message", message}
        });
        // TODO: report a proper error, IO error, bad certificate etc
        qCCritical(acme_client) << message << '\n'
            << certPaths.cert << '\n'
            << certPaths.key << '\n';
        return;
    }

    auto expiry = cert.getExpiryOrError();
    if(expiry.success) {
        handleRenewal(expiry.value, certPaths);
    } else {
        const char* message =  "Failed to read certificate expiry date.";
        setError(status["certificate"], "invalid", {
            {"message", message}
        });
        qCCritical(acme_client) << message << '\n';
        qCDebug(acme_client) << status.dump(1).c_str() << '\n';
        return;
    }

}

void DomainServerAcmeClient::handleRenewal(system_clock::time_point expiry, const CertificatePaths& certPaths) {
    status["certificate"]["status"] = "ok";
    status["certificate"]["expiry"] = secondsSinceEpoch(expiry).count();

    this->expiry = expiry;

    auto remaining = remainingTime(expiry);
    if(remaining > 0s) {
        scheduleRenewalIn(remaining);
    } else {
        generateCertificate(certPaths);
    }
}

void DomainServerAcmeClient::scheduleRenewalIn(system_clock::duration duration) {
    renewalTimer.stop();
    renewalTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
    auto sceduleTime = system_clock::now() + duration;
    status["certificate"]["renewal"] = secondsSinceEpoch(sceduleTime).count();
    qCDebug(acme_client) << "Renewal scheduled for:" << dateTimeFrom(sceduleTime);
}

bool DomainServerAcmeClient::handleAuthenticatedHTTPRequest(HTTPConnection *connection, const QUrl &url) {
    const QString URL_PREFIX = "/acme";
    const QString STATUS_URL = URL_PREFIX + "/status";
    const QString UPDATE_URL = URL_PREFIX + "/update";

    const QString ACCOUNT_KEY_URL = URL_PREFIX + "/account-key";
    const QString CERT_URL = URL_PREFIX + "/cert";
    const QString CERT_KEY_URL = URL_PREFIX + "/cert-key";
    const QString CERT_AUTHORITIES_URL = URL_PREFIX + "/cert-authorities";

    auto certPaths = getCertificatePaths(settings);
    const std::array<std::pair<QString, QString>, 4> fileMap {{
        {ACCOUNT_KEY_URL, getAccountKeyPath(settings)},
        {CERT_URL, certPaths.cert},
        {CERT_KEY_URL, certPaths.key},
        {CERT_AUTHORITIES_URL, certPaths.trustedAuthorities}
    }};

    if (connection->requestOperation() == QNetworkAccessManager::GetOperation) {
        if(url.path() == STATUS_URL) {
            connection->respond(
                HTTPConnection::StatusCode200,
                QByteArray::fromStdString(status.dump()),
                "application/json");
            return true;
        }
    } else if (connection->requestOperation() == QNetworkAccessManager::PostOperation) {
        if(url.path() == UPDATE_URL) {
            if(status["directory"] != "pending" &&
                status["account"] != "pending" &&
                status["certificate"] != "pending") {
                connection->respond(HTTPConnection::StatusCode200);
                init();
            } else {
                connection->respond(HTTPConnection::StatusCode409);
            }
            return true;
        }
    } else {
        auto file = std::find_if(fileMap.begin(), fileMap.end(), [&url](auto&& file)
            { return file.first == url.path(); });
        if(file != fileMap.end()) {
            auto filePath = file->second;
            if (connection->requestOperation() == QNetworkAccessManager::PutOperation) {
                if(QFile::exists(filePath)) {
                    connection->respond(HTTPConnection::StatusCode409);
                } else {
                    if(writeAll(connection->requestContent().toStdString(), filePath)) {
                        connection->respond(HTTPConnection::StatusCode200);
                    } else {
                        connection->respond(HTTPConnection::StatusCode500);
                    }
                }
                return true;
            } else if (connection->requestOperation() == QNetworkAccessManager::DeleteOperation) {
                if(QFile(filePath).remove()) {
                    connection->respond(HTTPConnection::StatusCode200);
                } else {
                    connection->respond(HTTPConnection::StatusCode500);
                }
                return true;
            }
        }
    }

    return false;
}
