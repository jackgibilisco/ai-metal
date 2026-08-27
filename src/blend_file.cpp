#include "blend_file.h"

#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

#pragma pack(push, 1)
struct LargeBHead8 {
    int32_t code;
    int32_t sdnaIndex;
    uint64_t oldPointer;
    int64_t length;
    int64_t count;
};
#pragma pack(pop)

int32_t MakeBlockCode(char a, char b, char c = 0, char d = 0) {
    return (int32_t)((uint8_t)a | ((uint8_t)b << 8) | ((uint8_t)c << 16) | ((uint8_t)d << 24));
}

struct SdnaMember {
    int16_t typeIndex;
    int16_t nameIndex;
};

struct SdnaStruct {
    int16_t typeIndex;
    std::vector<SdnaMember> members;
};

struct Sdna {
    std::vector<std::string> names;
    std::vector<std::string> types;
    std::vector<int16_t> typeSizes;
    std::vector<SdnaStruct> structs;
};

struct Block {
    int32_t code;
    int structIndex;
    const uint8_t *data;
    int64_t length;
    uint64_t oldPointer;
};

bool DecompressZstd(const std::vector<uint8_t> &compressed, std::vector<uint8_t> &out) {
    ZSTD_DStream *stream = ZSTD_createDStream();
    if (!stream) {
        return false;
    }
    ZSTD_initDStream(stream);

    ZSTD_inBuffer input = {compressed.data(), compressed.size(), 0};
    std::vector<uint8_t> chunk(1 << 20);
    size_t returnCode;
    do {
        ZSTD_outBuffer output = {chunk.data(), chunk.size(), 0};
        returnCode = ZSTD_decompressStream(stream, &output, &input);
        if (ZSTD_isError(returnCode)) {
            ZSTD_freeDStream(stream);
            return false;
        }
        out.insert(out.end(), chunk.data(), chunk.data() + output.pos);
    } while (input.pos < input.size);

    ZSTD_freeDStream(stream);
    return true;
}

bool ValidateHeader(const std::vector<uint8_t> &data) {
    if (data.size() < 17) {
        return false;
    }
    return memcmp(data.data(), "BLENDER", 7) == 0 && data[7] == '1' && data[8] == '7' &&
           data[9] == '-' && data[10] == '0' && data[11] == '1' && data[12] == 'v';
}

// See PLAN.md ("Feature: File > Import File...") for the format reference
// this was verified against (blender/blender's dna_genfile.cc).
bool ParseSdna(const uint8_t *data, int64_t length, Sdna &sdna) {
    const uint8_t *p = data;
    const uint8_t *end = data + length;

    auto atEnd = [&](int64_t bytes) { return p + bytes > end; };
    auto readInt = [&]() -> int32_t {
        int32_t v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    };
    auto readShort = [&]() -> int16_t {
        int16_t v;
        memcpy(&v, p, 2);
        p += 2;
        return v;
    };
    auto padUp4 = [&]() { p = data + (((p - data) + 3) & ~3); };

    if (atEnd(4) || memcmp(p, "SDNA", 4) != 0) return false;
    p += 4;

    if (atEnd(4) || memcmp(p, "NAME", 4) != 0) return false;
    p += 4;
    int32_t namesCount = readInt();
    sdna.names.reserve(namesCount);
    for (int i = 0; i < namesCount; ++i) {
        if (atEnd(1)) return false;
        const char *str = (const char *)p;
        size_t len = strnlen(str, end - p);
        sdna.names.emplace_back(str, len);
        p += len + 1;
    }
    padUp4();

    if (atEnd(4) || memcmp(p, "TYPE", 4) != 0) return false;
    p += 4;
    int32_t typesCount = readInt();
    sdna.types.reserve(typesCount);
    for (int i = 0; i < typesCount; ++i) {
        if (atEnd(1)) return false;
        const char *str = (const char *)p;
        size_t len = strnlen(str, end - p);
        sdna.types.emplace_back(str, len);
        p += len + 1;
    }
    padUp4();

    if (atEnd(4) || memcmp(p, "TLEN", 4) != 0) return false;
    p += 4;
    if (atEnd((int64_t)typesCount * 2)) return false;
    sdna.typeSizes.resize(typesCount);
    for (int i = 0; i < typesCount; ++i) sdna.typeSizes[i] = readShort();
    if (typesCount & 1) p += 2;

    if (atEnd(4) || memcmp(p, "STRC", 4) != 0) return false;
    p += 4;
    int32_t structsCount = readInt();
    sdna.structs.resize(structsCount);
    for (int i = 0; i < structsCount; ++i) {
        if (atEnd(4)) return false;
        SdnaStruct &s = sdna.structs[i];
        s.typeIndex = readShort();
        int16_t membersCount = readShort();
        if (atEnd((int64_t)membersCount * 4)) return false;
        s.members.resize(membersCount);
        for (int m = 0; m < membersCount; ++m) {
            s.members[m].typeIndex = readShort();
            s.members[m].nameIndex = readShort();
        }
    }
    return true;
}

int ArrayCountFromName(const std::string &name) {
    int count = 1;
    bool foundBracket = false;
    size_t pos = 0;
    while ((pos = name.find('[', pos)) != std::string::npos) {
        foundBracket = true;
        count *= atoi(name.c_str() + pos + 1);
        pos += 1;
    }
    return foundBracket ? count : 1;
}

// DNA structs never have compiler-inserted padding between members (this is
// a hard requirement enforced on Blender's own struct authors), so summing
// member sizes in declaration order gives the correct byte offset.
int64_t MemberByteSize(const Sdna &sdna, const SdnaMember &member) {
    const std::string &name = sdna.names[member.nameIndex];
    if (!name.empty() && name[0] == '*') {
        return 8; // format version 1 files always use 8-byte pointers.
    }
    return (int64_t)sdna.typeSizes[member.typeIndex] * ArrayCountFromName(name);
}

bool FindMember(const Sdna &sdna, int structIndex, const std::string &wantName,
                 int64_t *outOffset, int64_t *outSize) {
    if (structIndex < 0 || structIndex >= (int)sdna.structs.size()) {
        return false;
    }
    int64_t offset = 0;
    for (const SdnaMember &member : sdna.structs[structIndex].members) {
        int64_t size = MemberByteSize(sdna, member);
        if (sdna.names[member.nameIndex] == wantName) {
            *outOffset = offset;
            *outSize = size;
            return true;
        }
        offset += size;
    }
    return false;
}

} // namespace

struct BlendFile {
    std::vector<uint8_t> data;
    Sdna sdna;
    std::vector<Block> blocks;
};

BlendFile *BlendFileOpen(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long compressedSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (compressedSize < 0) {
        fclose(f);
        return nullptr;
    }
    std::vector<uint8_t> compressed(compressedSize);
    size_t bytesRead = compressedSize > 0 ? fread(compressed.data(), 1, compressedSize, f) : 0;
    fclose(f);
    if (bytesRead != (size_t)compressedSize) {
        return nullptr;
    }

    std::vector<uint8_t> decompressed;
    if (compressed.size() >= 7 && memcmp(compressed.data(), "BLENDER", 7) == 0) {
        decompressed = std::move(compressed); // already-uncompressed .blend
    } else if (!DecompressZstd(compressed, decompressed)) {
        return nullptr;
    }

    if (!ValidateHeader(decompressed)) {
        fprintf(stderr, "blend_file: unsupported or invalid .blend header\n");
        return nullptr;
    }

    BlendFile *file = new BlendFile();
    file->data = std::move(decompressed);

    size_t cursor = 17;
    while (cursor + sizeof(LargeBHead8) <= file->data.size()) {
        LargeBHead8 header;
        memcpy(&header, file->data.data() + cursor, sizeof(header));
        cursor += sizeof(header);
        if (header.code == MakeBlockCode('E', 'N', 'D', 'B')) {
            break;
        }
        if (header.length < 0 || cursor + (uint64_t)header.length > file->data.size()) {
            fprintf(stderr, "blend_file: corrupt block length\n");
            delete file;
            return nullptr;
        }

        Block block;
        block.code = header.code;
        block.structIndex = header.sdnaIndex;
        block.data = file->data.data() + cursor;
        block.length = header.length;
        block.oldPointer = header.oldPointer;
        file->blocks.push_back(block);

        cursor += header.length;
    }

    for (const Block &block : file->blocks) {
        if (block.code == MakeBlockCode('D', 'N', 'A', '1')) {
            if (!ParseSdna(block.data, block.length, file->sdna)) {
                fprintf(stderr, "blend_file: failed to parse SDNA\n");
                delete file;
                return nullptr;
            }
            break;
        }
    }
    if (file->sdna.structs.empty()) {
        fprintf(stderr, "blend_file: no DNA1 block found\n");
        delete file;
        return nullptr;
    }

    return file;
}

void BlendFileClose(BlendFile *file) {
    delete file;
}

const void *BlendFileNextBlock(BlendFile *file, const char *idCode,
                                const void *previousBlock, int *outStructIndex) {
    int32_t wantCode = MakeBlockCode(idCode[0], idCode[1]);
    size_t startIndex = 0;
    if (previousBlock) {
        for (size_t i = 0; i < file->blocks.size(); ++i) {
            if (file->blocks[i].data == previousBlock) {
                startIndex = i + 1;
                break;
            }
        }
    }
    for (size_t i = startIndex; i < file->blocks.size(); ++i) {
        if (file->blocks[i].code == wantCode) {
            if (outStructIndex) *outStructIndex = file->blocks[i].structIndex;
            return file->blocks[i].data;
        }
    }
    return nullptr;
}

bool BlendFileReadFloatArray(BlendFile *file, const void *structData, int structIndex,
                              const char *fieldName, float *out, int count) {
    int64_t offset, size;
    if (!FindMember(file->sdna, structIndex, fieldName, &offset, &size)) {
        return false;
    }
    if (size < (int64_t)sizeof(float) * count) {
        return false;
    }
    memcpy(out, (const uint8_t *)structData + offset, sizeof(float) * count);
    return true;
}

bool BlendFileReadInt(BlendFile *file, const void *structData, int structIndex,
                       const char *fieldName, int32_t *out) {
    int64_t offset, size;
    if (!FindMember(file->sdna, structIndex, fieldName, &offset, &size)) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)structData + offset;
    switch (size) {
        case 1: *out = *(const int8_t *)p; return true;
        case 2: *out = *(const int16_t *)p; return true;
        case 4: *out = *(const int32_t *)p; return true;
        case 8: *out = (int32_t)*(const int64_t *)p; return true;
        default: return false;
    }
}

const void *BlendFileFollowPointer(BlendFile *file, const void *structData, int structIndex,
                                    const char *fieldName, int *outStructIndex) {
    int64_t offset, size;
    std::string starName = std::string("*") + fieldName;
    if (!FindMember(file->sdna, structIndex, starName, &offset, &size) || size != 8) {
        return nullptr;
    }
    uint64_t pointerValue;
    memcpy(&pointerValue, (const uint8_t *)structData + offset, 8);
    if (pointerValue == 0) {
        return nullptr;
    }
    for (const Block &block : file->blocks) {
        if (block.oldPointer == pointerValue) {
            if (outStructIndex) *outStructIndex = block.structIndex;
            return block.data;
        }
    }
    return nullptr;
}
