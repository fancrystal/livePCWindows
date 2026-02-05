// encryption_utils.h
#ifndef ENCRYPTION_UTILS_H
#define ENCRYPTION_UTILS_H

#include <QString>
#include <windows.h>
#include <wincrypt.h>

const int AES_BLOCK_SIZE = 16;

class EncryptionUtils
{
public:
    static QString encryptAES128_ECB(const QString &data, const QString &key);
    static QString decryptAES128_ECB(const QString &data, const QString &key);

private:
    static bool cryptDataECB(bool encrypt, const QByteArray &input, QByteArray &output, const QByteArray &key);
};

#endif // ENCRYPTION_UTILS_H
