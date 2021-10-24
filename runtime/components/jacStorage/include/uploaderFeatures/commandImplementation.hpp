#pragma once

#include <uploader.hpp>

#include <string>
#include <iostream>
#include <sstream>
#include <memory>
#include <mbedtls/base64.h>

// There are missing guards, fixed in
// https://github.com/espressif/esp-idf/commit/cbf207bfb83156ece449a10908cad0615d66ec52
extern "C" {
    #include <esp_vfs.h>
    #include <esp_vfs_fat.h>
    #include <esp_system.h>
}

#include <filesystem.hpp>
#include <jacUtility.hpp>

namespace jac::storage {

using namespace std::string_literals;

template < typename Self >
class CommandImplementation {
public:
    Self& self() {
        return *static_cast< Self* >( this );
    }

    const Self& self() const {
        return *static_cast< const Self* >( this );
    }

    void doList( const std::string& prefix ) {
        using namespace jac::fs;
        const int prefixLen = strlen( getStoragePrefix() ) + 1;
        listDirectory( getStoragePrefix() + prefix,
            [&]( FileType type, const std::string& path, const std::string& entityName ) {
                if ( jac::utility::startswith( entityName, "__" ) )
                    return;
                if ( type == FileType::Directory )
                    self().yieldString( "D" );
                else if ( type == FileType::File )
                    self().yieldString( "F" );
                else
                    self().yieldString( "?" );

                std::stringstream ss;
                ss << " " << std::string_view( path ).substr( prefixLen )
                          << "/" << entityName << "\n";
                self().yieldString( ss.str() );
            },
            [&]( const std::string& error ) {
                self().yieldError( error );
            });
            self().yieldString( "\n" );

    }

    void doPull( const std::string& filename ) {
        const std::string path = fsPath( filename );

        const int CHUNK_SIZE = 1023;
        static_assert( CHUNK_SIZE % 3 == 0 );
        const int ENCODED_SIZE = 4 * ( CHUNK_SIZE + 2 ) / 3 + 1;
        std::unique_ptr< unsigned char[] > fileBuffer( new unsigned char[ CHUNK_SIZE ] );
        std::unique_ptr< unsigned char[] > encBuffer( new unsigned char[ ENCODED_SIZE ] );
        int fd = open( path.c_str(), O_RDONLY );
        if ( fd < 0 ) {
            self().yieldError( std::strerror( errno ) );
            return;
        }
        int bytesRead;
        while ( ( bytesRead = read( fd, fileBuffer.get(), CHUNK_SIZE ) ) ) {
            size_t processed;
            int result = mbedtls_base64_encode(
                encBuffer.get(), ENCODED_SIZE, &processed,
                fileBuffer.get(), bytesRead );
            assert( result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL );
            self().yieldBuffer( reinterpret_cast< uint8_t * >( encBuffer.get() ), processed );
        }
        self().yieldString( "\n" );

        close( fd );
    }

    void doRemove( const std::string& filename ) {
        const auto filePath = fsPath( filename );
        if ( remove( filePath.c_str() ) < 0 )
            self().yieldError( std::strerror( errno ) );
        self().yieldString( "OK\n" );
    }

    void startFilePush() {
        if ( _workingFd >= 0 )
            close( _workingFd );
        _workingFd = open( workingFilename().c_str(), O_TRUNC | O_WRONLY | O_CREAT );
        if ( _workingFd < 0 ) {
            self().yieldError( std::strerror( errno ) );
            return;
        }
    }

    void addFileChunk( unsigned char* buffer, int size ) {
        assert( _workingFd >= 0 );
        int written = write( _workingFd, buffer, size );
        if ( written < 0 )
            self().yieldError( std::strerror( errno ) );
    }

    void commitFilePush( const std::string& filename ) {
        close( _workingFd );
        _workingFd = -1;

        auto path = fsPath( filename );
        if ( !jac::fs::ensurePath( path ) )
            self().yieldError( "Cannot create path " + path + ": " + std::strerror( errno ) );
        remove( path.c_str() );
        {
            int fd = open(workingFilename().c_str(), O_RDONLY);
            assert(fd >= 0);
            close(fd);
        }
        int res = rename( workingFilename().c_str(), path.c_str() );
        if ( res < 0 )
            self().yieldError( "Cannot finalize push: "s + std::strerror( errno ));
        self().yieldString( "OK\n" );
    }

    void performExit() {
        self().yieldString( "OK\n" );
        _finished = true;
    }

    void doStats() {
        // Source: https://github.com/espressif/esp-idf/issues/1660
        FATFS *fs;
        DWORD freeClusters;

        int res = f_getfree( "0:", &freeClusters, &fs );
        if ( res ) {
            self().yieldError( "Cannot determine free space" );
            return;
        }
        int totalSectors = (fs->n_fatent - 2) * fs->csize;
        int freeSectors = freeClusters * fs->csize;
        std::stringstream ss;
        ss << freeSectors * CONFIG_WL_SECTOR_SIZE << " "
            << totalSectors * CONFIG_WL_SECTOR_SIZE << "\n";
        self().yieldString( ss.str() );
    }

private:
    static std::string workingFilename() {
        return getStoragePrefix() + "/__tmp.txt"s;
    }

    static std::string fsPath( const std::string& filename ) {
        std::string path = getStoragePrefix();
        if ( filename.front() != '/' )
            path += "/";
        path += filename;
        return path;
    }

    bool _finished = false;
    int _workingFd = -1;
};

} // namespace jac::storage