#include <windows.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <array>
#include <string>
#include <CommCtrl.h>

class edid_sender {
private:
    HANDLE h_com_port;
    bool connected;
    std::string port_name;
    DWORD baud_rate;
    static const size_t chunk_size = 60;
    static constexpr std::array<uint8_t, 6> header = { 0xAA, 0x55, 0x00, 0x01, 0x01, 0x01 };
    static constexpr std::array<uint8_t, 1> trailer = { 0x5A };

public:
    edid_sender(const std::string& port)
        : port_name(port), connected(false), h_com_port(INVALID_HANDLE_VALUE) {}

    ~edid_sender() {
        if (connected && h_com_port != INVALID_HANDLE_VALUE) {
            CloseHandle(h_com_port);
        }
    }

    bool try_baud_rate(DWORD baud) {
        baud_rate = baud;
        if (!configure_port()) {
            return false;
        }
        return true;
    }

    bool open_port() {
        std::string full_port_name = "\\\\.\\" + port_name;

        h_com_port = CreateFileA(
            full_port_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (h_com_port == INVALID_HANDLE_VALUE) {
            std::cerr << "Failed to open port " << port_name << std::endl;
            return false;
        }

        connected = true;
        return true;
    }

    bool try_baudrate_sequence(const std::vector<uint8_t>& edid_data) {
        struct baud_config {
            DWORD rate;
            DWORD timeout;
        };

        const baud_config configs[] = {
            { 115200, 30000 },
            { 921600, 5000 }
        };

        for (const auto& config : configs) {
            std::cout << "Trying " << config.rate << " baud..." << std::endl;
            if (try_baud_rate(config.rate) && send_edid_with_timeout(edid_data, config.timeout)) {
                return true;
            }
        }

        return false;
    }

    bool send_edid(const std::string& edid_path) {
        if (!connected) return false;

        std::vector<uint8_t> edid_data(256, 0);
        std::ifstream file(edid_path, std::ios::binary);
        if (!file || !file.read(reinterpret_cast<char*>(edid_data.data()), 256)) {
            std::cerr << "Failed to read EDID file" << std::endl;
            return false;
        }
        file.close();

        return try_baudrate_sequence(edid_data);
    }

private:
    bool send_edid_with_timeout(const std::vector<uint8_t>& edid_data, DWORD timeout) {
        PurgeComm(h_com_port, PURGE_RXCLEAR | PURGE_TXCLEAR);

        std::vector<uint8_t> full_data;
        full_data.reserve(header.size() + edid_data.size() + trailer.size());
        full_data.insert(full_data.end(), header.begin(), header.end());
        full_data.insert(full_data.end(), edid_data.begin(), edid_data.end());
        full_data.insert(full_data.end(), trailer.begin(), trailer.end());

        // Send in 60-byte chunks
        size_t offset = 0;
        while (offset < full_data.size()) {
            size_t remaining = full_data.size() - offset;
            size_t chunk_size_actual = (remaining < chunk_size) ? remaining : chunk_size;

            DWORD bytes_written;
            if (!WriteFile(h_com_port, &full_data[offset], chunk_size_actual, &bytes_written, nullptr)) {
                std::cerr << "Failed to write chunk" << std::endl;
                return false;
            }

            if (bytes_written != chunk_size_actual) {
                std::cerr << "Incomplete write" << std::endl;
                return false;
            }

            offset += bytes_written;
            Sleep(10);
        }

        Sleep(100);

        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = timeout;
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        SetCommTimeouts(h_com_port, &timeouts);

        uint8_t response[7];
        DWORD bytes_read;
        if (!ReadFile(h_com_port, response, sizeof(response), &bytes_read, nullptr)) {
            return false;
        }

        const uint8_t expected[] = { 0xAA, 0x55, 0x0A, 0x00, 0x02, 0x4F, 0x4B };
        if (bytes_read == 7 && memcmp(response, expected, 7) == 0) {
            std::cout << "EDID injection successful! (Baud rate: " << baud_rate << ")" << std::endl;
            return true;
        }

        return false;
    }

    bool configure_port() {
        DCB dcb = { 0 };
        dcb.DCBlength = sizeof(DCB);

        if (!GetCommState(h_com_port, &dcb)) {
            std::cerr << "Error getting COM port state" << std::endl;
            return false;
        }

        dcb.BaudRate = baud_rate;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;

        if (!SetCommState(h_com_port, &dcb)) {
            std::cerr << "Error setting COM port state" << std::endl;
            return false;
        }

        SetupComm(h_com_port, 0x400, 0x400);

        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = 0;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 30000;
        timeouts.WriteTotalTimeoutMultiplier = 50;
        timeouts.WriteTotalTimeoutConstant = 10;

        if (!SetCommTimeouts(h_com_port, &timeouts)) {
            std::cerr << "Error setting timeouts" << std::endl;
            return false;
        }

        PurgeComm(h_com_port, 0xF);
        return true;
    }
};