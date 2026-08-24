#include <CameraRemote_SDK.h>

#include <iostream>

int main()
{
    const bool initialized = SCRSDK::Init();
    std::cout << "SCRSDK::Init=" << (initialized ? "true" : "false") << '\n';

    if (initialized) {
        SCRSDK::Release();
    }

    return initialized ? 0 : 1;
}
