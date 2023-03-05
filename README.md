### openjdk

```text
—— corba：不流行的多语言、分布式通讯接口
—— hotspot：Java 虚拟机源码
—— jaxp：XML 处理
—— jaxws：一组 XML web services 的 Java API
—— jdk：java 开发工具包
—— —— 针对操作系统的部分
—— —— share：与平台无关的实现
—— langtools：Java 语言工具
—— nashorn：JVM 上的 JavaScript 运行时
```

### 二分模型

`HotSpot`采用`oop-Klass`模型表示`Java`的对象和类。`oop`（`ordinary object pointer`）指普通的对象指针，`Klass`表示对象的具体类型。

为何要设计一个一分为二的对象模型呢？这是因为`HotSpot`的设计者不想让每个对象中都含有一个`vtable`（虚函数表），所以就把对象模型拆成`Klass`和`oop`。其中，`oop`中不含有任何虚函数，自然就没有虚函数表，而`Klass`中含有虚函数表，可以进行方法的分发。

### Klass

`Java`类通过`Klass`来表示。简单来说`Klass`就是`Java`类在`HotSpot`中的`C++`对等体，主要用于描述`Java`对象的具体类型。一般而言，`HotSpotVM`在加载`Class`文件时会在元数据区创建`Klass`，表示类的元数据，通过`Klass`可以获取类的常量池、字段和方法等信息。

![avatar](img/klass.png)

### oop

`Java`对象用`oop`来表示，在`Java`创建对象的时候创建。也就是说，在`Java`应用程序运行过程中每创建一个`Java`对象，在`HotSpot VM`内部都会创建一个`oop`实例来表示`Java`对象。

![avatar](img/oopdesc.png)

`markOopDesc`不是指`Java`对象，而是指`Java`对象的头信息，因此表示普通`Java`类对象的`instanceOopDesc`实例和表示数组对象的`objArrayOopDesc`与`typeArrayOopDesc`实例都含有`markOopDesc`实例。
