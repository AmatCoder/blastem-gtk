// ajunzip.c
//
// Based on JUnzip library v0.1.1 by Joonas Pihlajamaa (firstname.lastname@iki.fi)
// released into public domain. https://github.com/jokkebk/JUnzip
//
// Modified by Amatcoder.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"

#include <zlib.h>

#pragma pack(1)

typedef struct {
    uint32_t signature;
    uint16_t versionNeededToExtract; // unsupported
    uint16_t generalPurposeBitFlag; // unsupported
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength; // unsupported
} JZLocalFileHeader;

typedef struct {
    uint32_t signature;
    uint16_t versionMadeBy; // unsupported
    uint16_t versionNeededToExtract; // unsupported
    uint16_t generalPurposeBitFlag; // unsupported
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength; // unsupported
    uint16_t fileCommentLength; // unsupported
    uint16_t diskNumberStart; // unsupported
    uint16_t internalFileAttributes; // unsupported
    uint32_t externalFileAttributes; // unsupported
    uint32_t relativeOffsetOflocalHeader;
} JZGlobalFileHeader;

typedef struct  {
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t offset;
} JZFileHeader;

typedef struct  {
    uint32_t signature; // 0x06054b50
    uint16_t diskNumber; // unsupported
    uint16_t centralDirectoryDiskNumber; // unsupported
    uint16_t numEntriesThisDisk; // unsupported
    uint16_t numEntries;
    uint32_t centralDirectorySize;
    uint32_t centralDirectoryOffset;
    uint16_t zipCommentLength;
    // Followed by .ZIP file comment (variable size)
} JZEndRecord;

#define JZ_BUFFER_SIZE 65536

unsigned char jzBuffer[JZ_BUFFER_SIZE]; // limits maximum zip descriptor size

int check(char *filename, void *data, long bytes)
{
    system_media sm;
    sm.buffer = data;
    sm.size = bytes;

    char *ext = strrchr(filename, '.');
    if (ext) ext++;
    sm.extension = ext;

    return detect_system_type(&sm);
}

FILE *writeFile(char *filename, void *data, long bytes)
{
    FILE *out = tmpfile();
    int i;

    out = fopen(filename, "wb");
    fwrite(data, 1, bytes, out);
    freopen (filename, "rb", out);

    return out;
}

int jzReadLocalFileHeader(FILE *zip, JZFileHeader *header, char *filename, int len)
{
    JZLocalFileHeader localHeader;

    if(fread(&localHeader, 1, sizeof(JZLocalFileHeader), zip) <
            sizeof(JZLocalFileHeader))
        return Z_ERRNO;

    if(localHeader.signature != 0x04034B50)
        return Z_ERRNO;

    if(len) {
        if(localHeader.fileNameLength >= len)
            return Z_ERRNO; // filename cannot fit

        if(fread(filename, 1, localHeader.fileNameLength, zip) <
                localHeader.fileNameLength)
            return Z_ERRNO;

        filename[localHeader.fileNameLength] = '\0'; // NULL terminate
    } else { // skip filename
        if(fseek(zip, localHeader.fileNameLength, SEEK_CUR))
            return Z_ERRNO;
    }

    if(localHeader.extraFieldLength) {
        if(fseek(zip, localHeader.extraFieldLength, SEEK_CUR))
            return Z_ERRNO;
    }

    //if(localHeader.generalPurposeBitFlag)
    //    return Z_ERRNO; // Flags not supported

    if(localHeader.compressionMethod == 0 &&
            (localHeader.compressedSize != localHeader.uncompressedSize))
        return Z_ERRNO; // Method is "store" but sizes indicate otherwise, abort

    memcpy(header, &localHeader.compressionMethod, sizeof(JZFileHeader));
    header->offset = 0; // not used in local context

    return Z_OK;
}

int jzReadData(FILE *zip, JZFileHeader *header, void *buffer)
{
    unsigned char *bytes = (unsigned char *)buffer; // cast
    long compressedLeft, uncompressedLeft;
    z_stream strm;
    int ret;

    if(header->compressionMethod == 0) { // Store - just read it
        if(fread(buffer, 1, header->uncompressedSize, zip) <
                header->uncompressedSize || ferror(zip))
            return Z_ERRNO;
    } else if(header->compressionMethod == 8) { // Deflate - using zlib
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;

        strm.avail_in = 0;
        strm.next_in = Z_NULL;

        // Use inflateInit2 with negative window bits to indicate raw data
        if((ret = inflateInit2(&strm, -MAX_WBITS)) != Z_OK)
            return ret; // Zlib errors are negative

        // Inflate compressed data
        for(compressedLeft = header->compressedSize,
                uncompressedLeft = header->uncompressedSize;
                compressedLeft && uncompressedLeft && ret != Z_STREAM_END;
                compressedLeft -= strm.avail_in) {
            // Read next chunk
            strm.avail_in = fread(jzBuffer, 1,
                    (sizeof(jzBuffer) < compressedLeft) ?
                    sizeof(jzBuffer) : compressedLeft, zip);

            if(strm.avail_in == 0 || ferror(zip)) {
                inflateEnd(&strm);
                return Z_ERRNO;
            }

            strm.next_in = jzBuffer;
            strm.avail_out = uncompressedLeft;
            strm.next_out = bytes;

            compressedLeft -= strm.avail_in; // inflate will change avail_in

            ret = inflate(&strm, Z_NO_FLUSH);

            if(ret == Z_STREAM_ERROR) return ret; // shouldn't happen

            switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR;
                case Z_DATA_ERROR: case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    return ret;
            }

            bytes += uncompressedLeft - strm.avail_out; // bytes uncompressed
            uncompressedLeft = strm.avail_out;
        }

        inflateEnd(&strm);
    } else {
        return Z_ERRNO;
    }

    return Z_OK;
}

FILE* processFile(FILE *zip)
{
    JZFileHeader header;
    char filename[1024];
    unsigned char *data;
    FILE *out;

    if(jzReadLocalFileHeader(zip, &header, filename, sizeof(filename))) {
        printf("Couldn't read local file header!");
        return NULL;
    }

    if((data = (unsigned char *)malloc(header.uncompressedSize)) == NULL) {
        printf("Couldn't allocate memory!");
        return NULL;
    }

    //printf("Unziping %s, %d / %d bytes at offset %08X\n", filename,
    //        header.compressedSize, header.uncompressedSize, header.offset);

    if(jzReadData(zip, &header, data) != Z_OK) {
        printf("Couldn't read file data!");
        free(data);
        return NULL;
    }

    if (check(filename, data, header.uncompressedSize))
      out = writeFile(filename, data, header.uncompressedSize);
    else
      out = NULL;

    free(data); 

    return out;
}

FILE* record(FILE *zip, JZFileHeader *header)
{
    long offset;
    FILE *out;

    offset = ftell(zip);

    if(fseek(zip, header->offset, SEEK_SET)) {
        printf("Cannot seek in zip file!");
        return 0;
    }

    out = processFile(zip); // alters file offset

    fseek(zip, offset, SEEK_SET); // return to position

    return out;
}

FILE * jzReadCentralDirectory(FILE *zip, JZEndRecord *endRecord)
{
    JZGlobalFileHeader fileHeader;
    JZFileHeader header;
    int i;
    FILE *out;

    if(fseek(zip, endRecord->centralDirectoryOffset, SEEK_SET)) {
        printf("Cannot seek in zip file!");
        return NULL;
    }

    for(i=0; i<endRecord->numEntries; i++) {
        if(fread(&fileHeader, 1, sizeof(JZGlobalFileHeader), zip) <
                sizeof(JZGlobalFileHeader)) {
            printf("Couldn't read file header!");
            return NULL;
        }
    
        if(fileHeader.signature != 0x02014B50) {
            printf("Invalid file header signature!");
            return NULL;
        }
    
        if(fileHeader.fileNameLength + 1 >= JZ_BUFFER_SIZE) {
            printf("Too long file name!");
            return NULL;
        }
    
        if(fread(jzBuffer, 1, fileHeader.fileNameLength, zip) <
                fileHeader.fileNameLength) {
            printf("Couldn't read filename!");
            return NULL;
        }
    
        jzBuffer[fileHeader.fileNameLength] = '\0'; // NULL terminate
    char *test =jzBuffer;
        if(fseek(zip, fileHeader.extraFieldLength, SEEK_CUR) ||
                fseek(zip, fileHeader.fileCommentLength, SEEK_CUR)) {
            printf("Couldn't skip extra field or file comment!");
            return NULL;
        }
    
        // Construct JZFileHeader from global file header
        memcpy(&header, &fileHeader.compressionMethod, sizeof(header));
        header.offset = fileHeader.relativeOffsetOflocalHeader;
    
        out = record(zip, &header);
        if (out) break;
    }

    return out;
}

int jzReadEndRecord(FILE *zip, JZEndRecord *endRecord)
{
    long fileSize, readBytes, i;
    JZEndRecord *er;

    if(fseek(zip, 0, SEEK_END)) {
        printf("Couldn't go to end of zip file!");
        return Z_ERRNO;
    }

    if((fileSize = ftell(zip)) <= sizeof(JZEndRecord)) {
        printf("Too small file to be a zip!");
        return Z_ERRNO;
    }

    readBytes = (fileSize < sizeof(jzBuffer)) ? fileSize : sizeof(jzBuffer);

    if(fseek(zip, fileSize - readBytes, SEEK_SET)) {
        printf("Cannot seek in zip file!");
        return Z_ERRNO;
    }

    if(fread(jzBuffer, 1, readBytes, zip) < readBytes) {
        printf("Couldn't read end of zip file!");
        return Z_ERRNO;
    }

    // Naively assume signature can only be found in one place...
    for(i = readBytes - sizeof(JZEndRecord); i >= 0; i--) {
        er = (JZEndRecord *)(jzBuffer + i);
        if(er->signature == 0x06054B50)
            break;
    }

    if(i < 0) {
        //printf("End record signature not found in zip!");
        fseek(zip, 0, SEEK_SET);
        return Z_ERRNO;
    }

    memcpy(endRecord, er, sizeof(JZEndRecord));

    if(endRecord->diskNumber || endRecord->centralDirectoryDiskNumber ||
            endRecord->numEntries != endRecord->numEntriesThisDisk) {
        printf("Multifile zips not supported!");
        return Z_ERRNO;
    }

    return Z_OK;
}

FILE* ajunzip(char *filename)
{
    FILE *out;
    FILE *zip;
    JZEndRecord endRecord;

    zip = fopen(filename ,"rb");

    if (!zip) return NULL;

    if(jzReadEndRecord(zip, &endRecord)) {
		fseek(zip, 0, SEEK_END);
        long size = ftell (zip);
        rewind (zip);
        char *buffer = (char*) malloc (sizeof(char)*size);
        fread(buffer, 1, size, zip);

        if (check(filename, buffer, size))
          out = zip;
        else
          out =  NULL;

		fseek(zip, 0, SEEK_SET);
        free(buffer);
        return out;
    }

    out = jzReadCentralDirectory(zip, &endRecord);

    if (!out){
        printf("Couldn't read ZIP file central record.");
        out = NULL;
    }

    fclose(zip);

    return out;
}
