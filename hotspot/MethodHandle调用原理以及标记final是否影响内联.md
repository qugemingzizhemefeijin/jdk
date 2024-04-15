### MethodHandle是什么？

方法句柄`（MethodHandle）`是一个强类型的，能够被直接执行的引用。从作用上来看，方法句柄类似于反射中的`Method`类，是对要执行的方法的一个引用，可以简单理解为函数指针，它是一种更加底层的查找、调整和调用方法的机制。该引用可以指向常规的静态方法或者实例方法，也可以指向构造器或者字段。它调用时有两个方法`invoke`和`invokeExact`，后者要求参数类型与底层方法的参数完全匹配，前者则在有出入时做修改如包装类型。

#### 为什么MethodHandle.invoke()不用进行方法修饰符权限检查？

`MethodHandles.Lookup`的创建位置就可以在外部类中调用私有方法了说明方法句柄的访问权限不取决于方法句柄的创建位置，而是取决于`Lookup`对象的创建位置。句柄在设计的时候就将方法修饰符权限检查放在了通过`MethodHandles.Lookup`获取`MethodHandle`的时候，`MethodHandle`的调用不会进行权限检查。如果该句柄被多次调用的话，那么与反射调用相比，它无需重复权限检查的开销。

#### 为什么MethodHandle.invoke()明明是native方法为什么还可以被JIT内联优化？

`JIT`是否会进行优化是通过解释器执行字节码指令时收集的`profile`(运行时信息)所决定的。而`native`方法的方法体没有字节码指令，是直接通过本地方法栈执行的。所以`native`方法不能`JIT`优化不能被内联到`java`方法的调用侧。在`Hotspot`虚拟机的`JIT`编译器设计的时候就是针对字节码指令进行优化的。

而`MethodHandle.invoke()`方法的调用并不是利用`JNI`机制来进行`native`方法调用而是执行`invokehandle`指令，所以在解释器解释执行`invokehandle`字节码指令时可以收集的`profile`(运行时信息)，在运行一段时间后`JIT`可以根据收集的`profile`信息对其进行内联优化。整个调用过程始终在`java`方法栈中运行，没有用到本地方法栈。

在类加载时期，`jvm`会扫描类中定义的全部方法，对方法的字节码进行优化（重排序、重写等），其中会将调用`MethodHandle.invoke`方法的`invokevirtual`指令重写为`invokehandle`指令。所以`MethodHandle.invoke`方法的调用并不是利用`JNI`机制来进行`native`方法调用而是执行`invokehandle`指令。

#### 为什么MethodHandle声明时要被final修饰，否则性能会大大打折扣？

如果代理的方法调用次数少于`MethodHandleStatics::CUSTOMIZE_THRESHOLD（默认127）`次，则不会内联我们被调用的方法。超过了之后，会在调用链路中为该`MethodHandle`生成特有的适配器。适配器内部就会调用到`MethodHandle.invokeBasic()`方法。

在整个调用`Java`方法的链路中，关键就在`MethodHandle.invokeBasic()`是否可以内联。而其中重要的一个判断就是`MethodHandle`是否是常量，也就是被`final`标识。

```
// 类LambdaForm$MH由虚拟机生成，类里面主要有方法invokeExact_MT
  // “invokevirtual MethodHandle::invokeExact”实际运行的是下面的LambdaForm$MH::invokeExact_MT
  LambdaForm$MH::invokeExact_MT 被@ForceInline注解，所以它被强制内联
    Invokers::checkExactType 被@ForceInline注解，所以它被强制内联
      MethodHandle::type <待定>
      Invokers::newWrongMethodTypeException <待定>
    Invokers::checkCustomized 被@ForceInline注解，所以它被强制内联
      MethodHandleImpl::isCompileConstant <待定>
      Invokers::maybeCustomize 被@DontInline注解，一定不能内联
    MethodHandle::invokeBasic 这个方法实际运行的是下面的LambdaForm$DMH::invokeStatic。<待定>
    // 注意：虽然下面的内容被@ForceInline强制内联，但是也要看MethodHandle::invokeBasic是否能被内联
    // 如果MethodHandle::invokeBasic不能被内联，则下面的方法也就不会被内联
      // 类LambdaForm$DMH由虚拟机生成，类里面主要有方法invokeStatic
      LambdaForm$DMH::invokeStatic @ForceInline注解，所以它被强制内联
        DirectMethodHandle::internalMemberName 被@ForceInline注解，所以它被强制内联
        MethodHandle::linkToStatic <待定>
          FinalMethod::testFinal <待定> -> 此为自己写的Java方法
```

有些方法被`@ForceInline`注解，所以大概率会内联。这里只能说“大概率”，因为如果方法不满足一些条件，比如内联层级超过默认最大值，`@ForceInline`注解的方法也不会内联，不过调试我们的“测试代码”的时候没遇到，所以这里`@ForceInline`可以认为就是内联的。有些方法被`@DontInline`注解，所以一定不会内联。除去含有注解`@ForceInline`和`@DontInline`的方法，则剩下标注为“<待定>”的方法，这是我们需要重点关注的内容。其中`MethodHandle::invokeBasic`是否能内联，则是问题描述里面最关心的内容。待定的方法从树中单独抽出，如下所示：
```
MethodHandle::type <待定>
Invokers::newWrongMethodTypeException <待定>
MethodHandleImpl::isCompileConstant <待定>
MethodHandle::invokeBasic 这个方法实际运行的是下面的LambdaForm$DMH::invokeStatic。<待定>
MethodHandle::linkToStatic <待定>
FinalMethod::testFinal <待定> -> 此为自己写的Java方法
```

其中

- `MethodHandle::type`：满足`try_inline`和`try_inline_full`方法的所有要求，内联。
- `Invokers::newWrongMethodTypeException`：方法`try_inline_full`里面的判断`callee->code_size_for_inlining() > max_inline_size()`不通过，说明方法太大，不能内联。
- `MethodHandleImpl::isCompileConstant`：满足`try_inline`和`try_inline_full`方法的所有要求，内联。

那就剩下了`MethodHandle::invokeBasic`调用树了，而这个里面就`type->is_constant()`，如果是`final`就会被内联，否则包括`MethodHandle::linkToStatic`和`FinalMethod::testFinal`均不会内联，从而影响到性能。

#### 是否字段必须添加final？

`try_method_handle_inline`方法中会判断`type->is_constant()`，其实编译器不只是判断有无final。编译器认为下面的任一情况满足，一个字段即是常量：

- 字段有`final`修饰、且有`static`修饰、且不是`java.clang.System`类的`in\out\err`且不在类初始化器外被修改；
- 字段有`@Stable`注解且非空；
- 字段有`final`修饰且字段在一些信任包（比如包`java.lang.invoke`，具体的包列举在`ciField::trust_final_non_static_fields`方法里）的类里面；
- 字段是`CallSite`类的`target`字段、且不在实例初始化器（构造函数）外被修改。

字段是否为常量的说明在`ciField::is_constant`的注释里面，实现代码在`ciField::initialize_from`方法里。

参考文档：

[MethodHandle标记final影响内联问题？](https://www.zhihu.com/question/535373016/answer/2517526289?utm_id=0)

[JVM系列之：关于方法句柄的那些事](https://zhuanlan.zhihu.com/p/477873226)


