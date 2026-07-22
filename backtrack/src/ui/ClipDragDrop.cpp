#include "ui/UiHelpers.h"

#include <objbase.h>

namespace backtrack {

HRESULT STDMETHODCALLTYPE ClipFileDropSource::QueryInterface(REFIID iid, void** result) {
    if (!result) {
        return E_POINTER;
    }
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropSource) {
        *result = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ClipFileDropSource::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
}

ULONG STDMETHODCALLTYPE ClipFileDropSource::Release() {
    const LONG references = InterlockedDecrement(&references_);
    if (references == 0) {
        delete this;
    }
    return static_cast<ULONG>(references);
}

HRESULT STDMETHODCALLTYPE ClipFileDropSource::QueryContinueDrag(BOOL escapePressed, DWORD keyState) {
    if (escapePressed) {
        return DRAGDROP_S_CANCEL;
    }
    return (keyState & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
}

HRESULT STDMETHODCALLTYPE ClipFileDropSource::GiveFeedback(DWORD) {
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

} // namespace backtrack
