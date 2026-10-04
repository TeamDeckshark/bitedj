#pragma once

#include <QtGlobal>

class EncoderCallback {
  public:
    // writes to encoded audio to a stream, e.g., a file stream or broadcast stream
    virtual void write(const unsigned char *header, const unsigned char *body,
                       int headerLen, int bodyLen) = 0;
    // gets stream position
    virtual qint64 tell() = 0;
    // sets stream position
    virtual void seek(qint64 pos) = 0;
    // gets stream length
    virtual qint64 filelen() = 0;
};
