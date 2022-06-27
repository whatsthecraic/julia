/*
 * flight-recorder.cpp
 *
 *  Created on: 17 Jun 2022
 *      Author: Dean De Leo
 */

#include <cinttypes>
#include <cstdlib> // malloc, free
#include <cstring>
#include <iostream>
#include <libgen.h> // basename
#include <limits>
#include <new> // bad_alloc
#include <stdexcept> // runtime_error

namespace {

/**
 * Keep track of fixed amount of messages for debugging purposes. The class is not thread-safe.
 */
class FlightRecorder {
    // disable the copy ctors
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    // A single entry in the flight recorder
    struct Entry {
        const char* m_source; // source file, given by the macro __FILE__
        uint64_t m_line; // line originating the entry, as given by the macro __LINE__
        const char* m_function; // the function, as given by standard macro __FUNCTION__
    };

    uint64_t m_end; // last index (excl)
    const uint64_t m_capacity; // capacity of the array m_array
    bool m_is_full; // whether the data structure is completely filled
    Entry* m_array; // entries stored in the flight recorded
    char* m_buffer; // buffer containing `m_capacity * m_message_sz` bytes
    const uint64_t m_message_sz; // the max size of a message for an entry, in bytes

    // Convert the given absolute index to its actual position in the array
    uint64_t to_array_index(int64_t index) const;

public:
    /**
     * Create a new instance of the flight recorder
     * @param capacity max number of entries stored
     * @param message_sz max size of a message stored, allocated statically
     */
    FlightRecorder(uint64_t capacity = 64, uint64_t message_sz = 512);

    /**
     * Destructor
     */
    ~FlightRecorder();

    /**
     * Add an entry in the flight recorder. Store, by memcpy, the message directly in the buffer returned by
     * the method.
     * @param source the source file where the entry was recorded, given by the macro __FILE__
     * @param line the line originating the entry, given by the macro __LINE__
     * @param function the function originating the entry, given by the macro __FUNCTION__
     * @param out_buffer_sz if given, it will be set with the size of the buffer returned by the function
     * @return the buffer where to store the message related to the entry
     */
    char* insert_c_api(const char* source, uint64_t line, const char* function, uint64_t* out_buffer_sz = nullptr);

    /**
     * Retrieve the current amount of entries stored in the flight recorder
     */
    size_t size() const;

    /**
     * Retrieve the (static) capacity of the flight recorder, that is the max number of entries that can be stored.
     */
    size_t capacity() const;

    /**
     * Dump the content of flight recorder
     * @param num_entries max number of entries to stored
     */
    void dump(uint64_t num_entries = std::numeric_limits<uint64_t>::max()) const;
};

// Implementation
FlightRecorder::FlightRecorder(uint64_t capacity, uint64_t message_sz) : m_end(0), m_capacity(capacity), m_is_full(false),
        m_array(nullptr), m_buffer(nullptr), m_message_sz(message_sz){
    //if(capacity == 0) throw std::invalid_argument("capacity is 0");
    if(capacity == 0){ std::cerr << "ERROR: FlightRecorder::ctor, capacity is 0" << std::endl; std::exit(1); }
    m_array = (Entry*) calloc(capacity, sizeof(Entry));
    m_buffer = (char*) calloc(capacity * message_sz, sizeof(char));
    //if (m_array == nullptr || m_buffer == nullptr) throw std::bad_alloc();
    if (m_array == nullptr || m_buffer == nullptr) { std::cerr << "ERROR: FlightRecorder::ctor, bad_alloc" << std::endl; std::exit(1); }
}

FlightRecorder::~FlightRecorder(){
    free(m_array); m_array = nullptr;
    free(m_buffer); m_buffer = nullptr;
}

char* FlightRecorder::insert_c_api(const char* source, uint64_t line, const char* function, uint64_t* out_buffer_sz){
    m_array[m_end].m_source = source;
    m_array[m_end].m_line = line;
    m_array[m_end].m_function = function;
    char* buffer = m_buffer + m_end * m_message_sz;

    m_end++; // next slot
    if(m_end == m_capacity){ m_is_full = true; m_end = 0; }

    if(out_buffer_sz != nullptr){
        *out_buffer_sz = m_message_sz;
    }
    return buffer;
}

uint64_t FlightRecorder::to_array_index(int64_t index) const {
    //if( index >= (int64_t) size() || index < 0 ) throw std::runtime_error("Index out of bounds");
    if( index >= (int64_t) size() || index < 0 ) { std::cerr << "ERROR: FlightRecorder::to_array_index:: invalid index: " << index << std::endl; std::exit(1); }
    if (! m_is_full){
        return index;
    } else {
        if(index < static_cast<int64_t>( m_capacity - m_end )) {
            return m_end + index;
        } else {
            return index - (m_capacity - m_end);
        }
    }
}

size_t FlightRecorder::size() const {
    if(m_is_full){
        return capacity();
    } else {
        return m_end;
    }
}

size_t FlightRecorder::capacity() const {
    return m_capacity;
}

void FlightRecorder::dump(uint64_t max_num_entries) const{
    uint64_t num_entries = std::min(size(), max_num_entries);

    // header
    std::cout << "[FlightRecorder] ";
    if(size() == 0){
        std::cout << "empty";
    } else {
        std::cout << "num entries: " << num_entries;
    }
    std::cout << std::endl; // instead of "\n", to flush immediately to stdout

    for(uint64_t i = size() - num_entries, N = size(), j = num_entries -1; i < N; i++, j--){ // from oldest to newest
//    for(int64_t i = static_cast<int64_t>(size()) -1, N = i - num_entries, j = 0; i > N; i--, j++){ // from newest to oldest
        uint64_t k = to_array_index(i);
        Entry* entry = m_array + k;
//        std::string basename = std::filesystem::path( entry->m_line ).filename(); // C++17
        char* buffer_basename = strdup(entry->m_source);
        const char* basename_ = basename(buffer_basename);
        const char* message = m_buffer + m_message_sz * k;
        uint64_t message_sz = strlen(message);

        std::cout << "[" << j << "][" << basename_ << "::" << entry->m_line << ", fn: " << entry->m_function << "]";
        if(message_sz > 0){
            std::cout << " " << message;
        }
        std::cout << std::endl;  // instead of "\n", to flush immediately to stdout

        free(buffer_basename);
    }
}

} // Anonymous namespace


extern "C" { // disable fn mangling

// C API
static FlightRecorder g_flight_recorder(/* capacity */ 2048); // singleton instance of the FR
static bool g_flight_recorder_enabled = false;

void flight_recorder_enable(bool value){
    g_flight_recorder_enabled = value;
}

bool flight_recorder_insert(const char* source, uint64_t line, const char* function, char** out_buffer, uint64_t* out_buffer_sz){
    *out_buffer = nullptr;
    *out_buffer_sz = 0;

    if(g_flight_recorder_enabled){
        *out_buffer = g_flight_recorder.insert_c_api(source, line, function, out_buffer_sz);
    }

    return g_flight_recorder_enabled;
}

void flight_recorder_dump() {
    g_flight_recorder.dump();
}

void flight_recorder_dump_n(uint64_t n){
    g_flight_recorder.dump(n);
}

} // extern "C"

