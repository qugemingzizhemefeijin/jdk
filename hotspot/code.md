#### Java堆初始化

调用链：
```
JavaMain() java.c
InitializeJVM() java.c
JNI_CreateJavaVM( ) jni.cpp
Threads::create_vm () thread.cpp
init_globals() init.cpp
universe_init() universe.cpp
Universe::initialize_heap() universe.cpp
```