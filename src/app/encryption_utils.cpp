#include "app/encryption_utils.h"
#include "common/log.h"
#include <QByteArray>
#include <string>

bool EncryptionUtils::cryptDataECB(bool encrypt, const QByteArray &input, QByteArray &output, const QByteArray &key)
{
    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    HCRYPTHASH hHash = 0;
    bool success = false;

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        LOG_WARNING(std::string("CryptAcquireContext failed: ") + std::to_string(GetLastError()));
        return false;
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        LOG_WARNING(std::string("CryptCreateHash failed: ") + std::to_string(GetLastError()));
        CryptReleaseContext(hProv, 0);
        return false;
    }

    if (!CryptHashData(hHash, (BYTE*)key.constData(), key.size(), 0)) {
        LOG_WARNING(std::string("CryptHashData failed: ") + std::to_string(GetLastError()));
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    if (!CryptDeriveKey(hProv, CALG_AES_128, hHash, 0, &hKey)) {
        LOG_WARNING(std::string("CryptDeriveKey failed: ") + std::to_string(GetLastError()));
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    // Set ECB mode (no IV needed)
    DWORD dwMode = CRYPT_MODE_ECB;
    if (!CryptSetKeyParam(hKey, KP_MODE, (BYTE*)&dwMode, 0)) {
        LOG_WARNING(std::string("CryptSetKeyParam failed: ") + std::to_string(GetLastError()));
        CryptDestroyKey(hKey);
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return false;
    }

    DWORD dwDataLen = input.size();
    DWORD dwBufLen = dwDataLen;

    // For ECB, input must be multiple of block size (16 bytes for AES)
    if (encrypt) {
        int pad = AES_BLOCK_SIZE - (dwDataLen % AES_BLOCK_SIZE);
        dwBufLen = dwDataLen + pad;
    }

    output.resize(dwBufLen);
    memcpy(output.data(), input.constData(), dwDataLen);

    if (encrypt) {
        if (!CryptEncrypt(hKey, 0, TRUE, 0, (BYTE*)output.data(), &dwDataLen, dwBufLen)) {
            LOG_WARNING(std::string("CryptEncrypt failed: ") + std::to_string(GetLastError()));
        } else {
            output.resize(dwDataLen);
            success = true;
        }
    } else {
        if (!CryptDecrypt(hKey, 0, TRUE, 0, (BYTE*)output.data(), &dwDataLen)) {
            LOG_WARNING(std::string("CryptDecrypt failed: ") + std::to_string(GetLastError()));
        } else {
            output.resize(dwDataLen);
            success = true;
        }
    }

    CryptDestroyKey(hKey);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    return success;
}

QString EncryptionUtils::encryptAES128_ECB(const QString &data, const QString &key)
{
    QByteArray input = data.toUtf8();
    QByteArray output;
    QByteArray keyBytes = key.toUtf8().left(16); // Ensure key is 16 bytes for AES-128

    if (!cryptDataECB(true, input, output, keyBytes)) {
        return QString();
    }

    return output.toBase64(QByteArray::Base64UrlEncoding);
}

QString EncryptionUtils::decryptAES128_ECB(const QString &data, const QString &key)
{
    QByteArray input = QByteArray::fromBase64(data.toUtf8());
    QByteArray output;
    QByteArray keyBytes = key.toUtf8().left(16); // Ensure key is 16 bytes for AES-128

    if (!cryptDataECB(false, input, output, keyBytes)) {
        return QString();
    }

    return QString::fromUtf8(output);
}
