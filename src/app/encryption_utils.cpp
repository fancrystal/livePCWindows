#include "app/encryption_utils.h"
#include "common/log.h"
#include <QByteArray>
#include <string>

namespace {
constexpr const char* CBC_PREFIX = "cbc:v1:";
constexpr const char* DPAPI_PREFIX = "dpapi:v1:";

void releaseCrypto(HCRYPTPROV hProv, HCRYPTKEY hKey, HCRYPTHASH hHash)
{
    if (hKey) {
        CryptDestroyKey(hKey);
    }
    if (hHash) {
        CryptDestroyHash(hHash);
    }
    if (hProv) {
        CryptReleaseContext(hProv, 0);
    }
}

bool deriveAes128Key(HCRYPTPROV hProv, const QByteArray& key, HCRYPTKEY& hKey, HCRYPTHASH& hHash)
{
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        LOG_WARNING(std::string("CryptCreateHash failed: ") + std::to_string(GetLastError()));
        return false;
    }

    if (!CryptHashData(hHash, reinterpret_cast<const BYTE*>(key.constData()), key.size(), 0)) {
        LOG_WARNING(std::string("CryptHashData failed: ") + std::to_string(GetLastError()));
        return false;
    }

    if (!CryptDeriveKey(hProv, CALG_AES_128, hHash, 0, &hKey)) {
        LOG_WARNING(std::string("CryptDeriveKey failed: ") + std::to_string(GetLastError()));
        return false;
    }

    return true;
}

bool randomBytes(QByteArray& bytes)
{
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        LOG_WARNING(std::string("CryptAcquireContext failed: ") + std::to_string(GetLastError()));
        return false;
    }

    const bool ok = CryptGenRandom(hProv, bytes.size(), reinterpret_cast<BYTE*>(bytes.data()));
    if (!ok) {
        LOG_WARNING(std::string("CryptGenRandom failed: ") + std::to_string(GetLastError()));
    }
    CryptReleaseContext(hProv, 0);
    return ok;
}
}

bool EncryptionUtils::cryptDataCBC(bool encrypt, const QByteArray &input, QByteArray &output, const QByteArray &key, const QByteArray &iv)
{
    if (iv.size() != AES_BLOCK_SIZE || key.isEmpty()) {
        return false;
    }

    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    HCRYPTHASH hHash = 0;
    bool success = false;

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        LOG_WARNING(std::string("CryptAcquireContext failed: ") + std::to_string(GetLastError()));
        return false;
    }

    if (!deriveAes128Key(hProv, key, hKey, hHash)) {
        releaseCrypto(hProv, hKey, hHash);
        return false;
    }

    DWORD dwMode = CRYPT_MODE_CBC;
    if (!CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&dwMode), 0)) {
        LOG_WARNING(std::string("CryptSetKeyParam(KP_MODE) failed: ") + std::to_string(GetLastError()));
        releaseCrypto(hProv, hKey, hHash);
        return false;
    }

    QByteArray ivCopy = iv;
    if (!CryptSetKeyParam(hKey, KP_IV, reinterpret_cast<BYTE*>(ivCopy.data()), 0)) {
        LOG_WARNING(std::string("CryptSetKeyParam(KP_IV) failed: ") + std::to_string(GetLastError()));
        releaseCrypto(hProv, hKey, hHash);
        return false;
    }

    DWORD dwDataLen = static_cast<DWORD>(input.size());
    DWORD dwBufLen = encrypt ? dwDataLen + AES_BLOCK_SIZE : dwDataLen;
    output.resize(static_cast<int>(dwBufLen));
    if (dwDataLen > 0) {
        memcpy(output.data(), input.constData(), dwDataLen);
    }

    if (encrypt) {
        if (!CryptEncrypt(hKey, 0, TRUE, 0, reinterpret_cast<BYTE*>(output.data()), &dwDataLen, dwBufLen)) {
            LOG_WARNING(std::string("CryptEncrypt(CBC) failed: ") + std::to_string(GetLastError()));
        } else {
            output.resize(static_cast<int>(dwDataLen));
            success = true;
        }
    } else {
        if (!CryptDecrypt(hKey, 0, TRUE, 0, reinterpret_cast<BYTE*>(output.data()), &dwDataLen)) {
            LOG_WARNING(std::string("CryptDecrypt(CBC) failed: ") + std::to_string(GetLastError()));
        } else {
            output.resize(static_cast<int>(dwDataLen));
            success = true;
        }
    }

    releaseCrypto(hProv, hKey, hHash);
    return success;
}

bool EncryptionUtils::cryptDataECB(bool encrypt, const QByteArray &input, QByteArray &output, const QByteArray &key)
{
    if (key.isEmpty()) {
        return false;
    }

    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    HCRYPTHASH hHash = 0;
    bool success = false;

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        LOG_WARNING(std::string("CryptAcquireContext failed: ") + std::to_string(GetLastError()));
        return false;
    }

    if (!deriveAes128Key(hProv, key, hKey, hHash)) {
        releaseCrypto(hProv, hKey, hHash);
        return false;
    }

    DWORD dwMode = CRYPT_MODE_ECB;
    if (!CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&dwMode), 0)) {
        LOG_WARNING(std::string("CryptSetKeyParam(KP_MODE) failed: ") + std::to_string(GetLastError()));
        releaseCrypto(hProv, hKey, hHash);
        return false;
    }

    DWORD dwDataLen = static_cast<DWORD>(input.size());
    DWORD dwBufLen = encrypt ? dwDataLen + AES_BLOCK_SIZE : dwDataLen;
    output.resize(static_cast<int>(dwBufLen));
    if (dwDataLen > 0) {
        memcpy(output.data(), input.constData(), dwDataLen);
    }

    if (encrypt) {
        if (!CryptEncrypt(hKey, 0, TRUE, 0, reinterpret_cast<BYTE*>(output.data()), &dwDataLen, dwBufLen)) {
            LOG_WARNING(std::string("CryptEncrypt(ECB) failed: ") + std::to_string(GetLastError()));
        } else {
            output.resize(static_cast<int>(dwDataLen));
            success = true;
        }
    } else {
        if (!CryptDecrypt(hKey, 0, TRUE, 0, reinterpret_cast<BYTE*>(output.data()), &dwDataLen)) {
            LOG_WARNING(std::string("CryptDecrypt(ECB) failed: ") + std::to_string(GetLastError()));
        } else {
            output.resize(static_cast<int>(dwDataLen));
            success = true;
        }
    }

    releaseCrypto(hProv, hKey, hHash);
    return success;
}

QString EncryptionUtils::encryptAES128_ECB(const QString &data, const QString &key)
{
    QByteArray output;
    QByteArray keyBytes = key.toUtf8().left(16);

    if (!cryptDataECB(true, data.toUtf8(), output, keyBytes)) {
        return QString();
    }

    return QString::fromLatin1(output.toBase64(QByteArray::Base64UrlEncoding));
}

QString EncryptionUtils::decryptAES128_ECB(const QString &data, const QString &key)
{
    QByteArray output;
    QByteArray keyBytes = key.toUtf8().left(16);
    QByteArray input = QByteArray::fromBase64(data.toUtf8(), QByteArray::Base64UrlEncoding);

    if (!cryptDataECB(false, input, output, keyBytes)) {
        return QString();
    }

    return QString::fromUtf8(output);
}

QString EncryptionUtils::encryptAES128_CBC(const QString &data, const QString &key)
{
    QByteArray input = data.toUtf8();
    QByteArray output;
    QByteArray keyBytes = key.toUtf8().left(16);
    QByteArray iv(AES_BLOCK_SIZE, 0);

    if (!randomBytes(iv) || !cryptDataCBC(true, input, output, keyBytes, iv)) {
        return QString();
    }

    return QString::fromLatin1(CBC_PREFIX) + QString::fromLatin1((iv + output).toBase64(QByteArray::Base64UrlEncoding));
}

QString EncryptionUtils::decryptAES128_CBC(const QString &data, const QString &key)
{
    QString payload = data;
    if (payload.startsWith(QString::fromLatin1(CBC_PREFIX))) {
        payload = payload.mid(QString::fromLatin1(CBC_PREFIX).size());
    }

    QByteArray combined = QByteArray::fromBase64(payload.toUtf8(), QByteArray::Base64UrlEncoding);
    if (combined.size() <= AES_BLOCK_SIZE) {
        return QString();
    }

    QByteArray iv = combined.left(AES_BLOCK_SIZE);
    QByteArray cipherText = combined.mid(AES_BLOCK_SIZE);
    QByteArray output;
    QByteArray keyBytes = key.toUtf8().left(16);

    if (!cryptDataCBC(false, cipherText, output, keyBytes, iv)) {
        return QString();
    }

    return QString::fromUtf8(output);
}

QString EncryptionUtils::protectForCurrentUser(const QString &data)
{
    QByteArray plain = data.toUtf8();
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(plain.data());
    input.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB output = {};
    if (!CryptProtectData(&input, L"LiveAssistant login password", NULL, NULL, NULL, 0, &output)) {
        LOG_WARNING(std::string("CryptProtectData failed: ") + std::to_string(GetLastError()));
        return QString();
    }

    QByteArray protectedBytes(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return QString::fromLatin1(DPAPI_PREFIX) + QString::fromLatin1(protectedBytes.toBase64(QByteArray::Base64UrlEncoding));
}

QString EncryptionUtils::unprotectForCurrentUser(const QString &data)
{
    if (!data.startsWith(QString::fromLatin1(DPAPI_PREFIX))) {
        return QString();
    }

    QByteArray protectedBytes = QByteArray::fromBase64(
        data.mid(QString::fromLatin1(DPAPI_PREFIX).size()).toUtf8(),
        QByteArray::Base64UrlEncoding);
    if (protectedBytes.isEmpty()) {
        return QString();
    }

    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(protectedBytes.data());
    input.cbData = static_cast<DWORD>(protectedBytes.size());

    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, 0, &output)) {
        LOG_WARNING(std::string("CryptUnprotectData failed: ") + std::to_string(GetLastError()));
        return QString();
    }

    QByteArray plain(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return QString::fromUtf8(plain);
}
