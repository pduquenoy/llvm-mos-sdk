#include <cstdint>
#include <new>

/** Trivial implementation of __cxa_guard_acquire and __cxa_guard_release
 *  as described in the c++ Itanium ABI.
 *
 *  Calls to these functions are generated by the compiler when it needs
 *  to ensure one-time initialization of non-global static variables.
 *
 *  According to the Itanium C++ ABI standard, section 3.3.3:
 *  "As described in Section 2.8, certain objects with static storage duration
 * have associated guard variables used to support the requirement that they be
 * initialized exactly once, the first time the scope declaring them is entered.
 * An implementation that does not anticipate supporting multi-threading may
 * simply check the first byte (i.e., the byte with lowest address) of that
 * guard variable, initializing if and only if its value is zero, and then
 * setting it to a non-zero value."
 *
 * Since this is intended to be a simple single-threaded implementation, we take
 * the advice presented and just initialize the first byte of the 64-bit guard
 * object.  The remaining 7 bytes are unused (but required to be present by the
 * ABI standard)
 */

extern "C" __attribute__((weak)) int __cxa_guard_acquire(uint64_t *guard_object) {
    // Check if the initializer has run by testing the lowest-address
    // byte for a non-zero value.
    return (*reinterpret_cast<uint8_t *>(guard_object) == 0) ? 1 : 0;
}

extern "C" __attribute__((weak)) void __cxa_guard_release(uint64_t *guard_object) {
    // set the lowest address byte to a non-zero value.
    *reinterpret_cast<uint8_t *>(guard_object) = 1;
}

using FinalizerPtr = void (*)();

static void NoOpFinalize() {}

// Function pointers in .fini_array are invoked from _fini.
static __attribute__((retain, section(".fini_array"))) FinalizerPtr CxaFinalizer = NoOpFinalize;

namespace {

struct ExitFunctionStorage {
    void (*m_functionptr)(void *);
    void * m_userdata;

    void operator()() const {
        m_functionptr(m_userdata);
    }
};


/* Exit functions are registered in a singly-linked list of blocks of registrations.
 * Each block contains 32 exit registrations, and additional
 * space for registrations is allocated on the heap, as needed.*/
class RegistrationList {
private:

    // FnBlock is an array of function pointers and their arguments.
    // The logical "front" of the block is the last item appended to 
    // the array.
    struct FnBlock {
        static constexpr std::uint8_t BLOCK_SZ = 32;

        ExitFunctionStorage m_funcs[BLOCK_SZ];
        std::uint8_t m_sz;

        bool full() const { return m_sz == BLOCK_SZ; }

        void push_front(const ExitFunctionStorage &newfn) {
          m_funcs[m_sz++] = newfn;
        }

        void run_all() {
            for (std::uint8_t i = m_sz; i > 0; --i) {
                m_funcs[i-1]();
            }
        }
    };

    struct FnNode : public FnBlock {

        FnNode(FnBlock * next) : m_next{next} {}

        FnBlock * const m_next = nullptr;
    };

public:
    static bool push_front(const ExitFunctionStorage & new_exit) {

        auto & current_block = *m_list;

        if (!current_block.full()) {
            current_block.push_front(new_exit);
            return true;
        }
        else {
            const auto next_block = new (std::nothrow) FnNode{m_list};
            if (!next_block) {
                // not enough memory to allocate another exit function
                return false;
            }

            // link new block to front of list.
            next_block->push_front(new_exit);
            m_list = next_block;
            return true;
        }
    }

    static void run_all_exits() {
        while (m_list != &m_tail) {
            m_list->run_all();

            const auto current_node_ptr = static_cast<FnNode *>(m_list);
            const auto next_block = current_node_ptr->m_next;
            // current_node_ptr is leaked here. We are shutting down.
            m_list = next_block;
        }

        m_tail.run_all();
    }

private:

    // The initial base node allows 32 exit registrations (1 block)
    // of exit functions, without allocating.  So the minumum required
    // 32 exit functions will always be available.
    static FnBlock m_tail;
    static FnBlock * m_list;
};

// Static allocation of registration list.
RegistrationList::FnBlock RegistrationList::m_tail{};
RegistrationList::FnBlock * RegistrationList::m_list = &m_tail;
}

// Currently the compiler does not generate any calls to this __cxa_finalize.  Finalization
// is invoked from _fini which is called from exit();
static void __finalize_noargs() { RegistrationList::run_all_exits(); }

// atexit / finalize are implemented under the assumption that there is only a single 
// loaded binary, with no dynamic loading.  Therefore; the mechanism for holding a DSO
// handle (the third parameter to _cxa_atexit), is ignored.  
extern "C" int __cxa_atexit(void (*f)(void *), void *p, void * /* dso_handle */) {
    // Ensure the CxaFinalizer entry in the finalizer table is pointing to the 
    // real finalizer function.  This is done within __cxa_atexit to ensure that
    // only code that actually registers something with __cxa_atexit pays the cost
    // of having the __atexit mechanism.  
    CxaFinalizer = __finalize_noargs;

    // Return values equal to C/C++ at_exit() return value.
    return RegistrationList::push_front(ExitFunctionStorage{f, p}) ? 0 : -1;
}
