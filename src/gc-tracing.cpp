/*
 * gc_tracing.cpp
 *
 *  Created on: 1 Jun 2022
 *      Author: Dean De Leo
 */
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "flight-recorder.h"
#include "gc.h"

using namespace std;

static unordered_set<jl_value_t*> g_targets; // pointers that are being traced
static bool g_recording = false; // whether a recording is in progress

// Having it as a string. Freaking Julia macros.
std::string str_symbol_name(jl_sym_t* symbol){ // GLOBAL
    return jl_symbol_name(symbol); // so we can reach it from the debugger :-)
}

void break_into_debugger(){
    __asm__ __volatile__ ("int $3"); // Raise a SIGTRAP
}

namespace { // anon

// A single object (value) in the julia ecosystem
class Object {
    // The type of connection from parent to child
    enum class ReferenceType {
        UNKNOWN,
        ROOT, // some kind of special entry point in the julia ecosystem, e.g. a task or a main module
        MODULE_BINDING, // this is coming from the table of bindings of a module
        TASK, // the parent is a task and this is the top frame
        FRAME, // the parent is another frame, part of a stack
        ARRAY, // this is the value in the position m_index, the parent is the array
        FIELD, // this is field in an object, tuple or task. The member m_name is always set.
    };

    jl_value_t* m_pointer; // actual Julia pointer
    Object* m_parent; // the link to the parent node
    ReferenceType m_type_ref; // the type of link between the parent and the child
//    union { // the specific connection
        string m_name; // the name of the field for an object
        uint64_t m_index; // the position of the slot for an array
//    };

    // Record a pointer from a task to a frame
    void set_stacktrace0(ReferenceType reftype, jl_value_t* from, jl_value_t* to);

public:
    Object(jl_value_t* object) : m_pointer(object), m_parent(nullptr), m_type_ref(ReferenceType::UNKNOWN), m_index(0){

    }

    // Destructor
    ~Object(){ }

    // Record the object as a root with the given name
    void set_root(const char* name);

    // Record a binding from a module
    void set_parent(jl_module_t* module, const char* name);

    // Record the root pointer for a frame
    void set_parent(jl_task_t* task);

    // Record a binding from a frame in the stack
    void set_parent(jl_gcframe_t* frame);

    // Record an index from the array
    void set_parent(jl_value_t* array, uint64_t index);

    // Record an index from an object
    void set_parent(jl_value_t* object, uint64_t field_index, const string& field_name);

    // Dump the content of the object to a string
    string to_string() const {
        stringstream ss;
        ss << "pointer: " << m_pointer;
        return ss.str();
    }

    static void dump_ancestor_chain(const Object& object);
};

static unordered_map<jl_value_t*, Object> g_index; // all recorded objects

// Get a string representation of a Julia value
// copy and paste from the heap-snapshot
static string to_string(const jl_value_t* value){
    constexpr uint64_t buffer_sz = 4 * 1024 * 1024; // 4 MB

    ios_t str_;
    ios_mem(&str_, buffer_sz);
    JL_STREAM* str = (JL_STREAM*)&str_;

    jl_static_show(str, (jl_value_t*) value);

    string name = string((const char*)str_.buf, str_.size);
    ios_close(&str_);

    return name;
}

static void target_found(jl_value_t* candidate){
    if (g_targets.count(candidate) == 0) return; // nope
    const Object& item = g_index.find(candidate)->second;

    cout << "[target_found] " << item.to_string() << endl;
    Object::dump_ancestor_chain(item);
    cout << "\n";

    auto it = g_targets.find(candidate);
    g_targets.erase(it);
}

} // anonymous namespace

extern "C" {

void jl_gc_addptr(void* pointer){
    jl_value_t* object = (jl_value_t*) pointer;
    cout << "[jl_gc_addptr] recording: " << object << " of type " << jl_typeof(object) << endl;
    g_targets.insert(object);
}

void jl_gc_trace(){
    if(g_targets.empty()){
        cout << "[jl_gc_trace] There are no targets to trace..." << endl;
        return;
    } else {
        cout << "[jl_gc_trace] Checking for the following targets: \n";
        int i = 0;
        for(jl_value_t* pointer : g_targets){
            cout << "[" << i << "] pointer: " << pointer << "\n";
            i++;
        }
    }

    // reset the content of the index
    g_index.clear();
    g_index.reserve(1ull << 30);

    // copied from the heap snapshot tool: https://github.com/JuliaLang/julia/pull/42286/files :
    // Initialize the GC's heuristics, so that JL_GC_FULL will work correctly. :)
    while (gc_num.pause < 2) {
        jl_gc_collect(JL_GC_AUTO);
    }

    g_recording = true;
    flight_recorder_enable(true);

    // run the garbage collect
    auto t0 = chrono::steady_clock::now();
    cout << "[jl_gc_trace] running the garbage collector... " << endl;
    jl_gc_collect(JL_GC_FULL);
    auto t1 = chrono::steady_clock::now();
    cout << "[jl_gc_trace] GC executed in " << chrono::duration_cast<chrono::seconds>(t1 - t0).count() << "." << chrono::duration_cast<chrono::milliseconds>((t1 - t0) % 1000).count() << " secs...\n";

    // print the targets that were not found:
    if(!g_targets.empty()){
        cout << "[jl_gc_trace] the following pointers were not detected:\n";
        int i = 0;
        for(jl_value_t* pointer : g_targets){
            cout << "[" << i << "] pointer: " << pointer << "\n";
            i++;
        }
    } else {
        cout << "[jl_gc_trace] all targets were detected\n";
    }

    g_targets.clear();
    g_index.clear();
    g_recording = false;
    flight_recorder_enable(false);
    cout << "[jl_gc_trace] done" << endl;
}

int jl_gc_is_tracing_enabled() {
    return g_recording;
}

// Callbacks from the GC

// This is the main module, tasks or the TypeMap (which I'm still unsure what exactly is)
void gc_record_root(jl_value_t *root, const char *name){
    FR("root: %p, name: %s", (void*) root, name);
    if (!g_recording || root == nullptr) return;

    auto it = g_index.emplace(root, root);
    if (!it.second) return; // does the root already exist?
    FR("root inserted");

    it.first->second.set_root(name);
    target_found(root);
}

void gc_record_frame_to_object_edge(jl_gcframe_t *from, jl_value_t *to) {
    FR("from: %p, to: %p", (void*) from, (void*) to)
    if (!g_recording) return;

    auto it = g_index.emplace(to, to);
    if (!it.second) return; // did the object already exist?
    FR("item inserted");
    it.first->second.set_parent(from);

    target_found(to);
}

void gc_record_task_to_frame_edge(jl_task_t *from, jl_gcframe_t *to) {
    FR("task: %p, frame: %p", (void*) from, (void*) to);
    if (!g_recording) return;
    auto it = g_index.emplace((jl_value_t *) to, (jl_value_t *) to);
    if (!it.second) return; // did the object already exist?
    FR("item inserted");
    it.first->second.set_parent(from);
}

void gc_record_frame_to_frame_edge(jl_gcframe_t *from, jl_gcframe_t *to){
    FR("from: %p, to: %p", (void*) from, (void*) to);
    if (!g_recording) return;
    auto it = g_index.emplace((jl_value_t *) to, (jl_value_t *) to);
    if (!it.second) return; // did the object already exist?
    FR("item inserted");
    it.first->second.set_parent(from);
}

void gc_record_array_edge(jl_value_t *from, jl_value_t *to, size_t index){
    FR("from: %p, to: %p, index: %d", (void*) from, (void*) to, (int) index);
    if (!g_recording) return;
    auto it = g_index.emplace(to, to);
    if (!it.second) return; // did the object already exist?
    FR("item inserted");
    it.first->second.set_parent(from, index);
    target_found(to);
}

// Coming from the binding table of a module
void gc_record_module_edge(jl_module_t *from, jl_value_t *to, const char *name){
    FR("module: %p, value: %p, name: %s", (void*) from, (void*) to, name);
    if (!g_recording) return;
    auto it = g_index.emplace(to, to);
    if (!it.second) return; // did the object already exist?
    FR("item inserted");

    it.first->second.set_parent(from, name);
    target_found(to);
}

void gc_record_module_edge_globalref(jl_module_t *from, jl_value_t *to, const char *name) {
    if (!g_recording || to == nullptr) return;
    constexpr size_t buffer_sz = 512;
    char buffer[buffer_sz];
    snprintf(buffer, buffer_sz, "%s_globalref", name);
    gc_record_module_edge(from, to, buffer);
}

void gc_record_object_edge(jl_value_t * const from, jl_value_t * const to, void* const slot){
    FR("from: %p, to: %p, slot: %p", (void*) from, (void*) to, (void*) slot);
    if (!g_recording) return;
    auto it = g_index.emplace(to, to);
    if (!it.second) return; // did the object already exist?
    FR("item inserted");
    Object& object = it.first->second;

    int field_index0 = -1; // index of the field in the object `from'
    stringstream ss; // construct the field name, e.g. x.y.z
    uint8_t* data = reinterpret_cast<uint8_t*>( jl_data_ptr(from) ); // data area for the current object
    uint32_t byte_offset = reinterpret_cast<uint8_t*>(slot) - data;  // byte offset of the pointer from the content area of the value
    jl_datatype_t* dt = (jl_datatype_t*) jl_typeof(from);
    bool done = false; // guard to stop the loop, until we have not found the ptr
    bool error = false; // to break into the debugger
    uint32_t num_levels = 0; // keep track of the number of inlined structs
    do { // we could need multiple iterations to unwrap objects that are inlined inside others
        // find the matching field_index in the current object
        int num_fields = jl_datatype_nfields(dt);
        int field_index = num_fields - 1; // proceed backwards
        while(jl_field_offset(dt, field_index) > byte_offset){
            field_index --;
        }
        assert(field_index >= 0);

        // record the first field for the original object `from'
        if(num_levels == 0){ // first iteration
            field_index0 = field_index;
        }

        // get the field name
        if(jl_is_tuple_type(dt)){ // tuple components don't have field names and are only referred by index
            ss << "[" << field_index << "]";
        } else {
            jl_sym_t* sym_field_name = nullptr;
            if(jl_is_namedtuple_type(dt)){ // named tuples store their field names in the first entry of its type parameters
                jl_value_t* field_names = jl_tparam0(dt);
                if (jl_is_tuple(field_names)){
                    sym_field_name = (jl_sym_t*) jl_get_nth_field(field_names, field_index);
                } // else, what the heck is this?
            } else { // any other object with a field name
                jl_svec_t* field_names = jl_field_names(dt);
                sym_field_name = (jl_sym_t*) jl_svecref(field_names, field_index);
            }

            if(ss.tellp() > 0) ss << "."; // add the concatenation point, e.g. x.y
            if(sym_field_name != nullptr){
                ss << jl_symbol_name(sym_field_name);
            } else {
                ss << "<unknown field name>";
            }
        }

        // does this field contain a pointer ?
        //cout << "[check] field_index: " << field_index << ", dt: "<< dt << ", is pointer: " << jl_field_isptr(dt, field_index) << "\n";
        if(jl_field_isptr(dt, field_index)){ // bingo!
            done = true;

            // this check is only for debugging purposes
            jl_value_t* pointer = *(reinterpret_cast<jl_value_t**>(data + jl_field_offset(dt, field_index)));
            //cout << "data: " << (void*) data << ", field_index: " << field_index << ", field_offset: "<< jl_field_offset(dt, field_index) << ", pointer: " << pointer << ", to: " << to << endl;
            error = !(pointer == to);
        } else {
            jl_datatype_t* fdt = (jl_datatype_t*) jl_field_type(dt, field_index);
            if(jl_stored_inline((jl_value_t*) fdt)){ // this is an inline struct, recurse to the next level
                num_levels ++; // next iteration

                data = data + jl_field_offset(dt, field_index);
                byte_offset -= jl_field_offset(dt, field_index);
                dt = fdt;
            } else { // no idea what this is
                done = true;
                error = true;
            }
        }
    } while(!done);

    string field_name = ss.str();

    if(error /*|| num_levels > 0*/){ // DEBUG ONLY
        jl_value_t** data = jl_data_ptr(from);
        jl_datatype_t* dt = (jl_datatype_t*) jl_typeof(from);

        bool is_named_tuple = jl_is_namedtuple_type(dt);
        bool is_tuple = jl_is_tuple_type(dt);

        cout << "[gc_record_object_edge] from: " << from << ", to: " << to << ", slot: " << slot << ", byte_offset: " << ((uint8_t*) slot - reinterpret_cast<uint8_t*>( jl_data_ptr(from) ));
        cout << ", field_index: " << field_index0 << ", num fields: " << jl_datatype_nfields(dt);
        cout << ", field_name: " << field_name;
        cout << ", is named tuple: " << (is_named_tuple? "yes" : "no");
        cout << ", is tuple: " << (is_tuple? "yes" : "no");
        cout << ", type: " << to_string((jl_value_t*) dt) << " (" << dt << ")";
        cout << ", error: " << error;
        cout << endl;

        cout << "Pointers: " << dt->layout->npointers << "\n";
        for(size_t i = 0; i < dt->layout->npointers; i++){
            jl_value_t* ptr = data[ jl_ptr_offset(dt, (int) i) ];
            cout << "[" << i << "] ptr: " << ptr << ", byte_offset: " << jl_ptr_offset(dt, (int) i) << ", match: " << (ptr == to) << endl;
        }

        cout << "Fields: " << jl_nfields(from) << "\n";
        jl_svec_t* field_names = jl_field_names(dt);
        for(size_t i = 0, N = jl_nfields(from); i < N; i++){
            cout << "[" << i << "] ";
            if(!is_tuple && !is_named_tuple){
                jl_sym_t* sym_field_name = (jl_sym_t*) jl_svecref(field_names, i);
                string field_name = jl_symbol_name(sym_field_name);
                cout << "name: " << field_name;
            }

            cout << ", byte_offset: " <<  jl_field_offset(dt, i) << ", size: " << jl_field_size(dt, i) << ", is pointer: " << jl_field_isptr(dt, i);
            if(jl_field_isptr(dt, i)){
                jl_value_t* pointer = jl_get_nth_field_noalloc(from, i);
                cout << ", pointer: " << pointer;
            }
            jl_datatype_t* fdt = (jl_datatype_t*) jl_field_type(dt, i);
            cout << ", field type: " << to_string((jl_value_t*) fdt);
            cout << ", stored inline: " << jl_stored_inline((jl_value_t*) fdt);
            cout << ", primitive type: " << jl_is_primitivetype(fdt);
            cout << endl;

            if (jl_stored_inline((jl_value_t*) fdt)){
                bool is_tuple = jl_is_tuple_type(fdt);
                bool is_named_tuple = jl_is_namedtuple_type(fdt);

                cout << "\tSubtype: " << to_string((jl_value_t*) fdt) << ", is_named_tuple: " << is_named_tuple << ", is_tuple: " << is_tuple;
                if(is_tuple){ cout << ", number of values: " << jl_nparams(fdt); }
                cout << ", number of fields: " << jl_datatype_nfields(fdt) << "\n";



                for(size_t i = 0, N = jl_datatype_nfields(fdt); i < N; i++){
                    cout << "[" << i << "] ";
                    if(is_tuple){ // tuples don't have field names
                        jl_datatype_t* pdt = (jl_datatype_t*) jl_tparam(fdt, i);
                        cout << to_string((jl_value_t*) pdt) << " (" << pdt << ")";
                    } else if (is_named_tuple){
                        jl_value_t* field_names = jl_tparam0(dt);
                        jl_sym_t* sym_field_name = (jl_sym_t*) jl_get_nth_field(field_names, i);
                        string field_name = jl_symbol_name(sym_field_name);
                        cout << "field name: " << field_name;
                    } else { // field name
                        jl_svec_t* field_names = jl_field_names(fdt);
                        jl_sym_t* sym_field_name = (jl_sym_t*) jl_svecref(field_names, i);
                        string field_name = jl_symbol_name(sym_field_name);
                        cout << "field name: " << field_name;
                    }
                    cout << ", byte_offset: " <<  jl_field_offset(fdt, i) << ", size: " << jl_field_size(fdt, i) << ", is pointer: " << jl_field_isptr(fdt, i) << endl;

                }
            } // stored inline?
        }

        if(error){
            cout << "BREAK INTO DEBUGGER" << endl;
            break_into_debugger();
        }
    } // END OF DEBUG ONLY

    object.set_parent(from, field_index0, field_name);
    target_found(to);
}

void gc_record_internal_edge(jl_value_t *from, jl_value_t *to){
    FR("from: %p, to: %p", (void*) from, (void*) to);
    if (!g_recording) return;

    // nop ...
}

void gc_record_hidden_edge(jl_value_t *from, size_t bytes){
    FR("from: %p, bytes: %d", (void*) from, (int) bytes);
    if (!g_recording) return;

    // nop ...
}

} // extern "C"


#define STOP_AND_DEBUG \
        flight_recorder_dump_n(128); \
        cout << "[ERROR] Cannot find the parent for " << this->m_pointer << endl; \
        break_into_debugger(); \

namespace {

// Record a entry point to the GC (main module, task or type map)
void Object::set_root(const char* name){
    m_type_ref = ReferenceType::ROOT;
    m_parent = nullptr;
    m_name = name;
}

// Record a binding from a module
void Object::set_parent(jl_module_t* module, const char* name){
    m_type_ref = ReferenceType::MODULE_BINDING;
    m_name = name;
    auto it = g_index.find((jl_value_t*) module);
    if (it == g_index.end()){
        STOP_AND_DEBUG
    } else {
        m_parent = &(it->second);
    }
}

// Record the root pointer for a stack
void Object::set_parent(jl_task_t* task){
    m_type_ref = ReferenceType::TASK;
    auto it = g_index.find((jl_value_t*) task);
    if (it == g_index.end()){
        STOP_AND_DEBUG
    } else {
        m_parent = &(it->second);
    }
}

// Record a pointer from a parent in the stack
void Object::set_parent(jl_gcframe_t* frame){
    m_type_ref = ReferenceType::FRAME;
    auto it = g_index.find((jl_value_t*) frame);
    if (it == g_index.end()){
        STOP_AND_DEBUG
    } else {
        m_parent = &(it->second);
    }
}

// Array component
void Object::set_parent(jl_value_t* array, uint64_t index) {
    m_type_ref = ReferenceType::ARRAY;
    auto it = g_index.find(array);
    if (it == g_index.end()){
        STOP_AND_DEBUG
    } else {
        m_parent = &(it->second);
    }
    m_index = index;
}

// Objects
void Object::set_parent(jl_value_t* object, uint64_t field_index, const string& field_name){
    m_type_ref = ReferenceType::FIELD;
    auto it = g_index.find(object);
    if (it == g_index.end()){
        STOP_AND_DEBUG
    } else {
        m_parent = &(it->second);
    }

    m_index = field_index;
    m_name = field_name;
}

void Object::dump_ancestor_chain(const Object& object){
    const Object* current = &object;

    int i = 0;
    do {
        const Object* parent = current->m_parent;
        cout << "[" << i << "] ";
        if (current->m_type_ref == Object::ReferenceType::ROOT){
            cout << "<root>";
            parent = nullptr; // in case of self-loops
        } else if (parent == nullptr){
            cout << "<unknown>";
        } else {
            string parent_type = ::to_string((jl_value_t*) jl_typeof(parent->m_pointer));
            cout << parent->m_pointer << " ::" << parent_type;
        }
        cout << " -> ";
        bool insert_another_arrow = false;
        if (!current->m_name.empty()){
            cout << current->m_name;
            insert_another_arrow = true;
        }
        if (current->m_type_ref == Object::ReferenceType::ARRAY || current->m_type_ref == Object::ReferenceType::FIELD){
            cout << " (field position: " << current->m_index << ")";
            insert_another_arrow = true;
        }
        if (insert_another_arrow){
            cout << " -> ";
        }
        string current_type = ::to_string((jl_value_t*) jl_typeof(current->m_pointer));
        cout << current->m_pointer << " ::" << current_type;
        cout << endl;


        // next iteration
        current = parent;
        i++;
    } while (current != nullptr);

}

} // anon
