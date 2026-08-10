#include "CompanionListJsonWriter.h"

#include <cstdio>
#include <cstring>

namespace companionpeer {

void JsonListWriter::bufferByte(uint8_t b) {
  if (bufLen_ == kBufferSize) flush();
  buf_[bufLen_++] = b;
}

void JsonListWriter::flush() {
  if (bufLen_ == 0) return;
  if (!sink_.write(buf_, bufLen_)) ok_ = false;
  bufLen_ = 0;
}

void JsonListWriter::writeRaw(const char* s) {
  for (const char* p = s; *p != '\0'; ++p) bufferByte(static_cast<uint8_t>(*p));
}

void JsonListWriter::writeUint(uint32_t v) {
  char buf[11];
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v));
  writeRaw(buf);
}

void JsonListWriter::writeJsonString(const char* s, uint8_t len) {
  bufferByte('"');
  for (uint8_t i = 0; i < len; ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    switch (c) {
      case '"':
        writeRaw("\\\"");
        break;
      case '\\':
        writeRaw("\\\\");
        break;
      case '\n':
        writeRaw("\\n");
        break;
      case '\r':
        writeRaw("\\r");
        break;
      case '\t':
        writeRaw("\\t");
        break;
      default:
        if (c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          writeRaw(buf);
        } else {
          bufferByte(c);
        }
    }
  }
  bufferByte('"');
}

void JsonListWriter::onDocument(uint32_t revision) {
  writeRaw("{\"revision\":");
  writeUint(revision);
  writeRaw(",\"lists\":[");
}

void JsonListWriter::onListStart(uint16_t listId, const char* title, uint8_t titleLen) {
  if (listIndex_++ > 0) writeRaw(",");
  writeRaw("{\"listId\":");
  writeUint(listId);
  writeRaw(",\"title\":");
  writeJsonString(title, titleLen);
  writeRaw(",\"groups\":[");
  groupIndex_ = 0;
}

void JsonListWriter::onGroupStart(uint16_t groupId, const char* label, uint8_t labelLen) {
  if (groupIndex_++ > 0) writeRaw(",");
  writeRaw("{\"groupId\":");
  writeUint(groupId);
  writeRaw(",\"label\":");
  writeJsonString(label, labelLen);
  writeRaw(",\"items\":[");
  itemIndex_ = 0;
}

void JsonListWriter::onItem(uint16_t itemId, bool checked, const char* text, uint8_t textLen) {
  ++itemCount_;
  if (itemIndex_++ > 0) writeRaw(",");
  writeRaw("{\"itemId\":");
  writeUint(itemId);
  writeRaw(",\"text\":");
  writeJsonString(text, textLen);
  writeRaw(checked ? ",\"checked\":true}" : ",\"checked\":false}");
}

void JsonListWriter::onGroupEnd(uint16_t /*groupId*/) { writeRaw("]}"); }

void JsonListWriter::onListEnd(uint16_t /*listId*/) { writeRaw("]}"); }

void JsonListWriter::finish() {
  writeRaw("]}");
  flush();
}

}  // namespace companionpeer
