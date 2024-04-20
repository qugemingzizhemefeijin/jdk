/*
 * Copyright (c) 1997, 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_OOPS_KLASS_HPP
#define SHARE_VM_OOPS_KLASS_HPP

#include "memory/genOopClosures.hpp"
#include "memory/iterator.hpp"
#include "memory/memRegion.hpp"
#include "memory/specialized_oop_closures.hpp"
#include "oops/klassPS.hpp"
#include "oops/metadata.hpp"
#include "oops/oop.hpp"
#include "runtime/orderAccess.hpp"
#include "trace/traceMacros.hpp"
#include "utilities/accessFlags.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_ALL_GCS
#include "gc_implementation/concurrentMarkSweep/cmsOopClosures.hpp"
#include "gc_implementation/g1/g1OopClosures.hpp"
#include "gc_implementation/parNew/parOopClosures.hpp"
#endif // INCLUDE_ALL_GCS

//
// A Klass provides:
//  1: language level class object (method dictionary etc.)
//  2: provide vm dispatch behavior for the object
// Both functions are combined into one C++ class.

// One reason for the oop/klass dichotomy in the implementation is
// that we don't want a C++ vtbl pointer in every object.  Thus,
// normal oops don't have any virtual functions.  Instead, they
// forward all "virtual" functions to their klass, which does have
// a vtbl and does the C++ dispatch depending on the object's
// actual type.  (See oop.inline.hpp for some of the forwarding code.)
// ALL FUNCTIONS IMPLEMENTING THIS DISPATCH ARE PREFIXED WITH "oop_"!

//  Klass layout:
//    [C++ vtbl ptr  ] (contained in Metadata)
//    [layout_helper ]
//    [super_check_offset   ] for fast subtype checks
//    [name          ]
//    [secondary_super_cache] for fast subtype checks
//    [secondary_supers     ] array of 2ndary supertypes
//    [primary_supers 0]
//    [primary_supers 1]
//    [primary_supers 2]
//    ...
//    [primary_supers 7]
//    [java_mirror   ]
//    [super         ]
//    [subklass      ] first subclass
//    [next_sibling  ] link to chain additional subklasses
//    [next_link     ]
//    [class_loader_data]
//    [modifier_flags]
//    [access_flags  ]
//    [last_biased_lock_bulk_revocation_time] (64 bits)
//    [prototype_header]
//    [biased_lock_revocation_count]
//    [_modified_oops]
//    [_accumulated_modified_oops]
//    [trace_id]


// Forward declarations.
template <class T> class Array;
template <class T> class GrowableArray;
class ClassLoaderData;
class klassVtable;
class ParCompactionManager;
class KlassSizeStats;

// Klass -> [ InstanceKlass, ArrayKlass ]
// InstanceKlass -> [ InstanceRefKlass, InstanceMirrorKlass, InstanceClassLoaderKlass ]
// ArrayKlass -> [ ObjArrayKlass, TypeArrayKlass ]
class Klass : public Metadata {
  friend class VMStructs;
 protected:
  // note: put frequently-used fields together at start of klass structure
  // for better cache behavior (may not make much of a difference but sure won't hurt)
  enum { _primary_super_limit = 8 };

  // The "layout helper" is a combined descriptor of object layout.
  // For klasses which are neither instance nor array, the value is zero.
  //
  // For instances, layout helper is a positive number, the instance size.
  // This size is already passed through align_object_size and scaled to bytes.
  // The low order bit is set if instances of this class cannot be
  // allocated using the fastpath.
  //
  // For arrays, layout helper is a negative number, containing four
  // distinct bytes, as follows:
  //    MSB:[tag, hsz, ebt, log2(esz)]:LSB
  // where:
  //    tag is 0x80 if the elements are oops, 0xC0 if non-oops
  //    hsz is array header size in bytes (i.e., offset of first element)
  //    ebt is the BasicType of the elements
  //    esz is the element size in bytes
  // This packed word is arranged so as to be quickly unpacked by the
  // various fast paths that use the various subfields.
  //
  // The esz bits can be used directly by a SLL instruction, without masking.
  //
  // Note that the array-kind tag looks like 0x00 for instance klasses,
  // since their length in bytes is always less than 24Mb.
  //
  // Final note:  This comes first, immediately after C++ vtable,
  // because it is frequently queried.
  // 对象布局的综合描述符，如果不是InstanceKlass或ArrayKlass，值为0.
  // 如果是InstanceKlass或ArrayKlass，这个值是一个组合数据。
  // (1) 对于InstanceKlass而言，组合数字中包含表示对象的以字节位单位的内存占用量。
  //     由于InstanceKlass实例能够表示Java类，因为这里指的内存占用量是指这个Java类创建的对象所需要的内存。
  // (2) 对于ArrayKlass而言，组合数字中包含tag、hsize、etype和esize四部分，具体怎么组合和解析由子类实现。
  // hotspot/src/share/vm/oops/klass.cpp -> 调用函数如： Klass::array_layout_helper(BasicType etype)
  jint        _layout_helper;

  // The fields _super_check_offset, _secondary_super_cache, _secondary_supers
  // and _primary_supers all help make fast subtype checks.  See big discussion
  // in doc/server_compiler/checktype.txt
  //
  // Where to look to observe a supertype (it is &_secondary_super_cache for
  // secondary supers, else is &_primary_supers[depth()].
  // 快速查找supertype的一个偏移量，这个偏移量是相对于Klass实例起始地址的偏移量。
  // 如果当前类是IOException，那么这个属性就指向_primary_supers数组中存储的IOException的位置。
  // 当存储的类多于8个时，值与_secondary_super_cache相等。
  juint       _super_check_offset;

  // Class name.  Instance classes: java/lang/String, etc.  Array classes: [I,
  // [Ljava/lang/String;, etc.  Set to zero for all other kinds of classes.
  // 类名，如java/lang/String何[Ljava/lang/String等
  Symbol*     _name;

  // _primary_supers、 _super_check_offset、 _secondary_supers
  // 与_secondary_super_cache这几个属性完全是为了加快判定父子关系等逻辑而加入的。具体参见：Klass::initialize_supers(Klass* k, TRAPS)

  // -XX:FastSuperclassLimit = 3，通过 java -XX:+PrintFlagsFinal 未发现此参数，估计只能用于DEVELOP环境。
  // _primary_supers[Object,A,B]
  // _secondary_supers[C,D,Test,IA,IB]

  // is_subtype_of(Klass* k) 方法会利用以下保存的属性进行父子关系的判断。

  // Cache of last observed secondary supertype
  // Klass指针，保存上一次查询父类的结果
  Klass*      _secondary_super_cache;
  // Array of all secondary supertypes
  // Klass指针数组，一般存储Java类实现的接口，偶尔还会存储Java类及其父类（如果继承父类的深度超过了8个）
  // 如class Test1 不继承和实现任何接口和类，则Test1[][]的 _secondary_supers = Cloneable[接口], Serializable[接口],
  // [Ljava/lang/Cloneable [类，ACC_SYNTHETIC (不由用户代码生成)], [Ljava/io/Serializeable [类，ACC_SYNTHETIC (不由用户代码生成)]
  // 所有的数组类型必须实现Cloneable接口 (See JLS 20.1.5)
  // 0x000002701b400a70	0x0000000000000004			_secondary_supers.length = 4
  // 0x000002701b400a78	0x000002701b014210			Cloneable
  // 0x000002701b400a80	0x000002701b003220			Serializeable
  // 0x000002701b400a88	0x000002701b15d630			[Ljava/lang/Cloneable
  // 0x000002701b400a90	0x000002701b0bd290			[Ljava/io/Serializeable
  Array<Klass*>* _secondary_supers;
  // Ordered list of all primary supertypes
  // 代表这个类的父类，其类型是一个Klass指针数组，大小固定是8。例如，IOException是Exception的子类，而Exception又是Throwable的子类，
  // 因此表示IOException类的_primary_supers属性值为[Throwable,Exception,IOException]。
  // 如果继承链过长，即当前类加上继承的类多于8个（默认值，可通过命令更改）时，会将多出来的类存储到_secondary_supers数组中。
  // 如class Test1 不继承和实现任何接口和类，则Test1[][]的 _primary_supers = [[LTest1 [自身], [[Ljava/lang/Object, [Ljava/lang/Object, Object
  // 0x000002701b4008b8	0x000002701b001c00			Object
  // 0x000002701b4008c0	0x000002701b0bca60			[Ljava/lang/Object;
  // 0x000002701b4008c8	0x000002701b2b4c40			[[Ljava/lang/Object;
  // 0x000002701b4008d0	0x000002701b400890			[[LTest1;
  Klass*      _primary_supers[_primary_super_limit];
  // java/lang/Class instance mirroring this class
  // oopDesc*类型，保存的是当前Klass实例表示的Java类所对应的java.lang.Class对象，可以据此访问类的静态属性
  oop       _java_mirror;

  // 因为Java是单继承方式，因此可通过 _super、_subklass和_next_sibling属性直接找到当前类型的父类或所有子类型。

  // Superclass
  // Klass指针，指向Java类的直接父类，在Klass::initialize_supers函数中设置的，因为传参就必须传入super类的Klass
  Klass*      _super;

  // 下面两个属性是通过 Klass::append_to_sibling_list() 函数设置的。

  // First subclass (NULL if none); _subklass->next_sibling() is next one
  // Klass指针，指向Java类的直接子类，由于直接子类可能有多个，因此多个子类会通过_next_sibling连接起来
  Klass*      _subklass;
  // Sibling link (or NULL); links all subklasses of a klass
  // Klass指针，通过该属性可以获取当前类的下一个子类，可通过调用语句_subklass->next_sibling()获取_subklass的兄弟子类
  Klass*      _next_sibling;

  // All klasses loaded by a class loader are chained through these links
  // Klass指针，ClassLoader加载的下一个Klass
  Klass*      _next_link;

  // The VM's representation of the ClassLoader used to load this class.
  // Provide access the corresponding instance java.lang.ClassLoader.
  ClassLoaderData* _class_loader_data;

  jint        _modifier_flags;  // Processed access flags, for use by Class.getModifiers.
  // 保存Java类的修饰符，如private、final、static、abstract和native等
  AccessFlags _access_flags;    // Access flags. The class/interface distinction is stored here.

  // Biased locking implementation and statistics
  // (the 64-bit chunk goes first, to avoid some fragmentation)
  jlong    _last_biased_lock_bulk_revocation_time;
  // 在锁的实现过程中非重要。（具体是干嘛的呢？？？）
  markOop  _prototype_header;   // Used when biased locking is both enabled and disabled for this type
  jint     _biased_lock_revocation_count;

  TRACE_DEFINE_KLASS_TRACE_ID;

  // Remembered sets support for the oops in the klasses.
  jbyte _modified_oops;             // Card Table Equivalent (YC/CMS support)
  jbyte _accumulated_modified_oops; // Mod Union Equivalent (CMS support)

  // Constructor
  Klass();

  void* operator new(size_t size, ClassLoaderData* loader_data, size_t word_size, TRAPS) throw();

 public:
  bool is_klass() const volatile { return true; }

  // super
  Klass* super() const               { return _super; }
  void set_super(Klass* k)           { _super = k; }

  // initializes _super link, _primary_supers & _secondary_supers arrays
  void initialize_supers(Klass* k, TRAPS);
  void initialize_supers_impl1(Klass* k);
  void initialize_supers_impl2(Klass* k);

  // klass-specific helper for initializing _secondary_supers
  virtual GrowableArray<Klass*>* compute_secondary_supers(int num_extra_slots);

  // java_super is the Java-level super type as specified by Class.getSuperClass.
  virtual Klass* java_super() const  { return NULL; }

  juint    super_check_offset() const  { return _super_check_offset; }
  void set_super_check_offset(juint o) { _super_check_offset = o; }

  Klass* secondary_super_cache() const     { return _secondary_super_cache; }
  void set_secondary_super_cache(Klass* k) { _secondary_super_cache = k; }

  Array<Klass*>* secondary_supers() const { return _secondary_supers; }
  void set_secondary_supers(Array<Klass*>* k) { _secondary_supers = k; }

  // Return the element of the _super chain of the given depth.
  // If there is no such element, return either NULL or this.
  Klass* primary_super_of_depth(juint i) const {
    assert(i < primary_super_limit(), "oob");
    Klass* super = _primary_supers[i];
    assert(super == NULL || super->super_depth() == i, "correct display");
    return super;
  }

  // Can this klass be a primary super?  False for interfaces and arrays of
  // interfaces.  False also for arrays or classes with long super chains.
  bool can_be_primary_super() const {
    const juint secondary_offset = in_bytes(secondary_super_cache_offset());
    return super_check_offset() != secondary_offset;
  }
  virtual bool can_be_primary_super_slow() const;

  // Returns number of primary supers; may be a number in the inclusive range [0, primary_super_limit].
  juint super_depth() const {
    if (!can_be_primary_super()) {
      return primary_super_limit();
    } else {
      juint d = (super_check_offset() - in_bytes(primary_supers_offset())) / sizeof(Klass*);
      assert(d < primary_super_limit(), "oob");
      assert(_primary_supers[d] == this, "proper init");
      return d;
    }
  }

  // store an oop into a field of a Klass
  void klass_oop_store(oop* p, oop v);
  void klass_oop_store(volatile oop* p, oop v);

  // java mirror
  oop java_mirror() const              { return _java_mirror; }
  void set_java_mirror(oop m) { klass_oop_store(&_java_mirror, m); }

  // modifier flags
  jint modifier_flags() const          { return _modifier_flags; }
  void set_modifier_flags(jint flags)  { _modifier_flags = flags; }

  // size helper
  // 获取Klass类中定义的 _layout_helper 属性的值
  int layout_helper() const            { return _layout_helper; }
  void set_layout_helper(int lh)       { _layout_helper = lh; }

  // Note: for instances layout_helper() may include padding.
  // Use InstanceKlass::contains_field_offset to classify field offsets.

  // sub/superklass links
  InstanceKlass* superklass() const;
  Klass* subklass() const;
  Klass* next_sibling() const;
  void append_to_sibling_list();           // add newly created receiver to superklass' subklass list

  void set_next_link(Klass* k) { _next_link = k; }
  Klass* next_link() const { return _next_link; }   // The next klass defined by the class loader.

  // class loader data
  ClassLoaderData* class_loader_data() const               { return _class_loader_data; }
  void set_class_loader_data(ClassLoaderData* loader_data) {  _class_loader_data = loader_data; }

  // The Klasses are not placed in the Heap, so the Card Table or
  // the Mod Union Table can't be used to mark when klasses have modified oops.
  // The CT and MUT bits saves this information for the individual Klasses.
  void record_modified_oops()            { _modified_oops = 1; }
  void clear_modified_oops()             { _modified_oops = 0; }
  bool has_modified_oops()               { return _modified_oops == 1; }

  void accumulate_modified_oops()        { if (has_modified_oops()) _accumulated_modified_oops = 1; }
  void clear_accumulated_modified_oops() { _accumulated_modified_oops = 0; }
  bool has_accumulated_modified_oops()   { return _accumulated_modified_oops == 1; }

 protected:                                // internal accessors
  Klass* subklass_oop() const            { return _subklass; }
  Klass* next_sibling_oop() const        { return _next_sibling; }
  void     set_subklass(Klass* s);
  void     set_next_sibling(Klass* s);

 public:

  // Compiler support
  static ByteSize super_offset()                 { return in_ByteSize(offset_of(Klass, _super)); }
  static ByteSize super_check_offset_offset()    { return in_ByteSize(offset_of(Klass, _super_check_offset)); }
  static ByteSize primary_supers_offset()        { return in_ByteSize(offset_of(Klass, _primary_supers)); }
  static ByteSize secondary_super_cache_offset() { return in_ByteSize(offset_of(Klass, _secondary_super_cache)); }
  static ByteSize secondary_supers_offset()      { return in_ByteSize(offset_of(Klass, _secondary_supers)); }
  static ByteSize java_mirror_offset()           { return in_ByteSize(offset_of(Klass, _java_mirror)); }
  static ByteSize modifier_flags_offset()        { return in_ByteSize(offset_of(Klass, _modifier_flags)); }
  static ByteSize layout_helper_offset()         { return in_ByteSize(offset_of(Klass, _layout_helper)); }
  static ByteSize access_flags_offset()          { return in_ByteSize(offset_of(Klass, _access_flags)); }

  // Unpacking layout_helper:
  enum {
    _lh_neutral_value           = 0,  // neutral non-array non-instance value
    _lh_instance_slow_path_bit  = 0x01,
    _lh_log2_element_size_shift = BitsPerByte*0,
    _lh_log2_element_size_mask  = BitsPerLong-1,
    _lh_element_type_shift      = BitsPerByte*1,
    _lh_element_type_mask       = right_n_bits(BitsPerByte),  // shifted mask
    _lh_header_size_shift       = BitsPerByte*2,
    _lh_header_size_mask        = right_n_bits(BitsPerByte),  // shifted mask
    _lh_array_tag_bits          = 2,
    _lh_array_tag_shift         = BitsPerInt - _lh_array_tag_bits,
    _lh_array_tag_type_value    = ~0x00,  // 0xC0000000 >> 30
    _lh_array_tag_obj_value     = ~0x01   // 0x80000000 >> 30
  };

  static int layout_helper_size_in_bytes(jint lh) {
    assert(lh > (jint)_lh_neutral_value, "must be instance");
    return (int) lh & ~_lh_instance_slow_path_bit;
  }
  static bool layout_helper_needs_slow_path(jint lh) {
    assert(lh > (jint)_lh_neutral_value, "must be instance");
    return (lh & _lh_instance_slow_path_bit) != 0;
  }
  static bool layout_helper_is_instance(jint lh) {
    return (jint)lh > (jint)_lh_neutral_value;
  }
  static bool layout_helper_is_array(jint lh) {
    return (jint)lh < (jint)_lh_neutral_value;
  }
  static bool layout_helper_is_typeArray(jint lh) {
    // _lh_array_tag_type_value == (lh >> _lh_array_tag_shift);
    return (juint)lh >= (juint)(_lh_array_tag_type_value << _lh_array_tag_shift);
  }
  static bool layout_helper_is_objArray(jint lh) {
    // _lh_array_tag_obj_value == (lh >> _lh_array_tag_shift);
    return (jint)lh < (jint)(_lh_array_tag_type_value << _lh_array_tag_shift);
  }
  static int layout_helper_header_size(jint lh) {
    assert(lh < (jint)_lh_neutral_value, "must be array");
    int hsize = (lh >> _lh_header_size_shift) & _lh_header_size_mask;
    assert(hsize > 0 && hsize < (int)sizeof(oopDesc)*3, "sanity");
    return hsize;
  }
  static BasicType layout_helper_element_type(jint lh) {
    assert(lh < (jint)_lh_neutral_value, "must be array");
    int btvalue = (lh >> _lh_element_type_shift) & _lh_element_type_mask;
    assert(btvalue >= T_BOOLEAN && btvalue <= T_OBJECT, "sanity");
    return (BasicType) btvalue;
  }
  static int layout_helper_log2_element_size(jint lh) {
    assert(lh < (jint)_lh_neutral_value, "must be array");
    int l2esz = (lh >> _lh_log2_element_size_shift) & _lh_log2_element_size_mask;
    assert(l2esz <= LogBitsPerLong,
        err_msg("sanity. l2esz: 0x%x for lh: 0x%x", (uint)l2esz, (uint)lh));
    return l2esz;
  }
  // 最终计算出来的数组类型的_layout_helper值为负数， 因为最高位为1，
  // 而对象类型通常是一个正数， 这样就可以简单地通过判断_layout_helper值来区分数组和对象。
  // 元素类型为对象类型的数组 | 10 | hsize (14位) | etype (8位) | log2(esize) (8位) |
  // 元素类型为基本类型的数组 | 11 | hsize (14位) | etype (8位) | log2(esize) (8位) |
  static jint array_layout_helper(jint tag, int hsize, BasicType etype, int log2_esize) {
    return (tag        << _lh_array_tag_shift)
      |    (hsize      << _lh_header_size_shift)
      |    ((int)etype << _lh_element_type_shift)
      |    (log2_esize << _lh_log2_element_size_shift);
  }

  // 计算 _layout_helper 的值，此方法在 parseClassFile()函数的时候会计算实例的大小。
  // 这个方法会左移3位，那么在 layout_helper_to_size_helper() 函数中相应的就是右移3位。
  // 为什么需要左移3位，然后 layout_helper_to_size_helper() 函数 右移3位呢？？？
  // 好像是因为这个内存实例大小的最后3位存储用于实例慢速分配有关的标志。
  // layout_helper_to_size_helper()中右移就会被丢弃掉，得到实例需要的真正内存大小。
  static jint instance_layout_helper(jint size, bool slow_path_flag) {
    // 保存时， size值左移3位， 低3位用来保存其他信息
    return (size << LogHeapWordSize)
      |    (slow_path_flag ? _lh_instance_slow_path_bit : 0); // 实例慢速分配有关
  }
  // 获取对象所需的内存空间
  static int layout_helper_to_size_helper(jint lh) {
    assert(lh > (jint)_lh_neutral_value, "must be instance");
    // Note that the following expression discards _lh_instance_slow_path_bit.
    // 64位LogHeapWordSize=3,32位LogHeapWordSize=2
    // LogHeapWordSize的值为3，向右移3位
    return lh >> LogHeapWordSize;
  }
  // Out-of-line version computes everything based on the etype:
  static jint array_layout_helper(BasicType etype);

  // What is the maximum number of primary superclasses any klass can have?
#ifdef PRODUCT
  static juint primary_super_limit()         { return _primary_super_limit; }
#else
  static juint primary_super_limit() {
    assert(FastSuperclassLimit <= _primary_super_limit, "parameter oob");
    return FastSuperclassLimit;
  }
#endif

  // vtables
  virtual klassVtable* vtable() const        { return NULL; }
  virtual int vtable_length() const          { return 0; }

  // subclass check
  bool is_subclass_of(const Klass* k) const;
  // subtype check: true if is_subclass_of, or if k is interface and receiver implements it
  // 判断当前类是否为k的子类。k可能为接口，如果当前类型实现了k接口，函数也返回true
  bool is_subtype_of(Klass* k) const {
    // 返回k的类在 _primary_supers 中的位置
    juint    off = k->super_check_offset();
    Klass* sup = *(Klass**)( (address)this + off );
    // 获取 _secondary_supers 的偏移位置
    // 一个类的继承联如果超过8个，那么其本身自己肯定在 在 _secondary_supers 中。
    // 并且其 _super_check_offset == secondary_offset。
    // secondary_super_cache_offset()是直接获取偏移
    const juint secondary_offset = in_bytes(secondary_super_cache_offset());
    // 如果k在_primary_supers中， 那么利用_primary_supers一定能判断出k与当前类的父子关系
    if (sup == k) {
      return true;
    // 如果k存储在_secondary_supers中， 那么当前类也肯定存储在_secondary_supers中如果两者有父子关系，
    // 那么_super_check_offset需要与_secondary_super_cache相等
    } else if (off != secondary_offset) {
      return false;
    } else {
      // 可能有父子关系， 需要进一步判断（父类不在子类的_primary_supers的属性中）
      // 这里会循环搜索 secondary_supers
      return search_secondary_supers(k);
    }
  }
  bool search_secondary_supers(Klass* k) const;

  // Find LCA in class hierarchy
  Klass *LCA( Klass *k );

  // Check whether reflection/jni/jvm code is allowed to instantiate this class;
  // if not, throw either an Error or an Exception.
  virtual void check_valid_for_instantiation(bool throwError, TRAPS);

  // array copying
  virtual void  copy_array(arrayOop s, int src_pos, arrayOop d, int dst_pos, int length, TRAPS);

  // tells if the class should be initialized
  virtual bool should_be_initialized() const    { return false; }
  // initializes the klass
  virtual void initialize(TRAPS);
  // lookup operation for MethodLookupCache
  friend class MethodLookupCache;
  virtual Method* uncached_lookup_method(Symbol* name, Symbol* signature) const;
 public:
  Method* lookup_method(Symbol* name, Symbol* signature) const {
    return uncached_lookup_method(name, signature);
  }

  // array class with specific rank
  Klass* array_klass(int rank, TRAPS)         {  return array_klass_impl(false, rank, THREAD); }

  // array class with this klass as element type
  Klass* array_klass(TRAPS)                   {  return array_klass_impl(false, THREAD); }

  // These will return NULL instead of allocating on the heap:
  // NB: these can block for a mutex, like other functions with TRAPS arg.
  Klass* array_klass_or_null(int rank);
  Klass* array_klass_or_null();

  virtual oop protection_domain() const = 0;

  oop class_loader() const;

  virtual oop klass_holder() const      { return class_loader(); }

 protected:
  virtual Klass* array_klass_impl(bool or_null, int rank, TRAPS);
  virtual Klass* array_klass_impl(bool or_null, TRAPS);

 public:
  // CDS support - remove and restore oops from metadata. Oops are not shared.
  virtual void remove_unshareable_info();
  virtual void restore_unshareable_info(TRAPS);

 protected:
  // computes the subtype relationship
  virtual bool compute_is_subtype_of(Klass* k);
 public:
  // subclass accessor (here for convenience; undefined for non-klass objects)
  virtual bool is_leaf_class() const { fatal("not a class"); return false; }
 public:
  // ALL FUNCTIONS BELOW THIS POINT ARE DISPATCHED FROM AN OOP
  // These functions describe behavior for the oop not the KLASS.

  // actual oop size of obj in memory
  virtual int oop_size(oop obj) const = 0;

  // Size of klass in word size.
  virtual int size() const = 0;
#if INCLUDE_SERVICES
  virtual void collect_statistics(KlassSizeStats *sz) const;
#endif

  // Returns the Java name for a class (Resource allocated)
  // For arrays, this returns the name of the element with a leading '['.
  // For classes, this returns the name with the package separators
  //     turned into '.'s.
  const char* external_name() const;
  // Returns the name for a class (Resource allocated) as the class
  // would appear in a signature.
  // For arrays, this returns the name of the element with a leading '['.
  // For classes, this returns the name with a leading 'L' and a trailing ';'
  //     and the package separators as '/'.
  virtual const char* signature_name() const;

  // garbage collection support
  virtual void oop_follow_contents(oop obj) = 0;
  virtual int  oop_adjust_pointers(oop obj) = 0;

  // Parallel Scavenge and Parallel Old
  PARALLEL_GC_DECLS_PV

  // type testing operations
 protected:
  virtual bool oop_is_instance_slow()       const { return false; }
  virtual bool oop_is_array_slow()          const { return false; }
  virtual bool oop_is_objArray_slow()       const { return false; }
  virtual bool oop_is_typeArray_slow()      const { return false; }
 public:
  virtual bool oop_is_instanceMirror()      const { return false; }
  virtual bool oop_is_instanceRef()         const { return false; }

  // Fast non-virtual versions
  #ifndef ASSERT
  #define assert_same_query(xval, xcheck) xval
  #else
 private:
  static bool assert_same_query(bool xval, bool xslow) {
    assert(xval == xslow, "slow and fast queries agree");
    return xval;
  }
 public:
  #endif
  inline  bool oop_is_instance()            const { return assert_same_query(
                                                    layout_helper_is_instance(layout_helper()),
                                                    oop_is_instance_slow()); }
  inline  bool oop_is_array()               const { return assert_same_query(
                                                    layout_helper_is_array(layout_helper()),
                                                    oop_is_array_slow()); }
  inline  bool oop_is_objArray()            const { return assert_same_query(
                                                    layout_helper_is_objArray(layout_helper()),
                                                    oop_is_objArray_slow()); }
  inline  bool oop_is_typeArray()           const { return assert_same_query(
                                                    layout_helper_is_typeArray(layout_helper()),
                                                    oop_is_typeArray_slow()); }
  #undef assert_same_query

  // Access flags
  AccessFlags access_flags() const         { return _access_flags;  }
  void set_access_flags(AccessFlags flags) { _access_flags = flags; }

  bool is_public() const                { return _access_flags.is_public(); }
  bool is_final() const                 { return _access_flags.is_final(); }
  bool is_interface() const             { return _access_flags.is_interface(); }
  bool is_abstract() const              { return _access_flags.is_abstract(); }
  bool is_super() const                 { return _access_flags.is_super(); }
  bool is_synthetic() const             { return _access_flags.is_synthetic(); }
  void set_is_synthetic()               { _access_flags.set_is_synthetic(); }
  bool has_finalizer() const            { return _access_flags.has_finalizer(); }
  bool has_final_method() const         { return _access_flags.has_final_method(); }
  void set_has_finalizer()              { _access_flags.set_has_finalizer(); }
  void set_has_final_method()           { _access_flags.set_has_final_method(); }
  bool is_cloneable() const             { return _access_flags.is_cloneable(); }
  void set_is_cloneable()               { _access_flags.set_is_cloneable(); }
  bool has_vanilla_constructor() const  { return _access_flags.has_vanilla_constructor(); }
  void set_has_vanilla_constructor()    { _access_flags.set_has_vanilla_constructor(); }
  bool has_miranda_methods () const     { return access_flags().has_miranda_methods(); }
  void set_has_miranda_methods()        { _access_flags.set_has_miranda_methods(); }

  // Biased locking support
  // Note: the prototype header is always set up to be at least the
  // prototype markOop. If biased locking is enabled it may further be
  // biasable and have an epoch.
  markOop prototype_header() const      { return _prototype_header; }
  // NOTE: once instances of this klass are floating around in the
  // system, this header must only be updated at a safepoint.
  // NOTE 2: currently we only ever set the prototype header to the
  // biasable prototype for instanceKlasses. There is no technical
  // reason why it could not be done for arrayKlasses aside from
  // wanting to reduce the initial scope of this optimization. There
  // are potential problems in setting the bias pattern for
  // JVM-internal oops.
  inline void set_prototype_header(markOop header);
  static ByteSize prototype_header_offset() { return in_ByteSize(offset_of(Klass, _prototype_header)); }

  int  biased_lock_revocation_count() const { return (int) _biased_lock_revocation_count; }
  // Atomically increments biased_lock_revocation_count and returns updated value
  int atomic_incr_biased_lock_revocation_count();
  void set_biased_lock_revocation_count(int val) { _biased_lock_revocation_count = (jint) val; }
  jlong last_biased_lock_bulk_revocation_time() { return _last_biased_lock_bulk_revocation_time; }
  void  set_last_biased_lock_bulk_revocation_time(jlong cur_time) { _last_biased_lock_bulk_revocation_time = cur_time; }

  TRACE_DEFINE_KLASS_METHODS;

  // garbage collection support
  virtual void oops_do(OopClosure* cl);

  // Iff the class loader (or mirror for anonymous classes) is alive the
  // Klass is considered alive.
  // The is_alive closure passed in depends on the Garbage Collector used.
  bool is_loader_alive(BoolObjectClosure* is_alive);

  static void clean_weak_klass_links(BoolObjectClosure* is_alive);

  // Prefetch within oop iterators.  This is a macro because we
  // can't guarantee that the compiler will inline it.  In 64-bit
  // it generally doesn't.  Signature is
  //
  // static void prefetch_beyond(oop* const start,
  //                             oop* const end,
  //                             const intx foffset,
  //                             const Prefetch::style pstyle);
#define prefetch_beyond(start, end, foffset, pstyle) {   \
    const intx foffset_ = (foffset);                     \
    const Prefetch::style pstyle_ = (pstyle);            \
    assert(foffset_ > 0, "prefetch beyond, not behind"); \
    if (pstyle_ != Prefetch::do_none) {                  \
      oop* ref = (start);                                \
      if (ref < (end)) {                                 \
        switch (pstyle_) {                               \
        case Prefetch::do_read:                          \
          Prefetch::read(*ref, foffset_);                \
          break;                                         \
        case Prefetch::do_write:                         \
          Prefetch::write(*ref, foffset_);               \
          break;                                         \
        default:                                         \
          ShouldNotReachHere();                          \
          break;                                         \
        }                                                \
      }                                                  \
    }                                                    \
  }

  // iterators
  virtual int oop_oop_iterate(oop obj, ExtendedOopClosure* blk) = 0;
  virtual int oop_oop_iterate_v(oop obj, ExtendedOopClosure* blk) {
    return oop_oop_iterate(obj, blk);
  }

#if INCLUDE_ALL_GCS
  // In case we don't have a specialized backward scanner use forward
  // iteration.
  virtual int oop_oop_iterate_backwards_v(oop obj, ExtendedOopClosure* blk) {
    return oop_oop_iterate_v(obj, blk);
  }
#endif // INCLUDE_ALL_GCS

  // Iterates "blk" over all the oops in "obj" (of type "this") within "mr".
  // (I don't see why the _m should be required, but without it the Solaris
  // C++ gives warning messages about overridings of the "oop_oop_iterate"
  // defined above "hiding" this virtual function.  (DLD, 6/20/00)) */
  virtual int oop_oop_iterate_m(oop obj, ExtendedOopClosure* blk, MemRegion mr) = 0;
  virtual int oop_oop_iterate_v_m(oop obj, ExtendedOopClosure* blk, MemRegion mr) {
    return oop_oop_iterate_m(obj, blk, mr);
  }

  // Versions of the above iterators specialized to particular subtypes
  // of OopClosure, to avoid closure virtual calls.
#define Klass_OOP_OOP_ITERATE_DECL(OopClosureType, nv_suffix)                \
  virtual int oop_oop_iterate##nv_suffix(oop obj, OopClosureType* blk) {     \
    /* Default implementation reverts to general version. */                 \
    return oop_oop_iterate(obj, blk);                                        \
  }                                                                          \
                                                                             \
  /* Iterates "blk" over all the oops in "obj" (of type "this") within "mr". \
     (I don't see why the _m should be required, but without it the Solaris  \
     C++ gives warning messages about overridings of the "oop_oop_iterate"   \
     defined above "hiding" this virtual function.  (DLD, 6/20/00)) */       \
  virtual int oop_oop_iterate##nv_suffix##_m(oop obj,                        \
                                             OopClosureType* blk,            \
                                             MemRegion mr) {                 \
    return oop_oop_iterate_m(obj, blk, mr);                                  \
  }

  SPECIALIZED_OOP_OOP_ITERATE_CLOSURES_1(Klass_OOP_OOP_ITERATE_DECL)
  SPECIALIZED_OOP_OOP_ITERATE_CLOSURES_2(Klass_OOP_OOP_ITERATE_DECL)

#if INCLUDE_ALL_GCS
#define Klass_OOP_OOP_ITERATE_BACKWARDS_DECL(OopClosureType, nv_suffix)      \
  virtual int oop_oop_iterate_backwards##nv_suffix(oop obj,                  \
                                                   OopClosureType* blk) {    \
    /* Default implementation reverts to general version. */                 \
    return oop_oop_iterate_backwards_v(obj, blk);                            \
  }

  SPECIALIZED_OOP_OOP_ITERATE_CLOSURES_1(Klass_OOP_OOP_ITERATE_BACKWARDS_DECL)
  SPECIALIZED_OOP_OOP_ITERATE_CLOSURES_2(Klass_OOP_OOP_ITERATE_BACKWARDS_DECL)
#endif // INCLUDE_ALL_GCS

  virtual void array_klasses_do(void f(Klass* k)) {}

  // Return self, except for abstract classes with exactly 1
  // implementor.  Then return the 1 concrete implementation.
  Klass *up_cast_abstract();

  // klass name
  Symbol* name() const                   { return _name; }
  void set_name(Symbol* n);

 public:
  // jvm support
  virtual jint compute_modifier_flags(TRAPS) const;

  // JVMTI support
  virtual jint jvmti_class_status() const;

  // Printing
  virtual void print_on(outputStream* st) const;

  virtual void oop_print_value_on(oop obj, outputStream* st);
  virtual void oop_print_on      (oop obj, outputStream* st);

  virtual const char* internal_name() const = 0;

  // Verification
  virtual void verify_on(outputStream* st, bool check_dictionary);
  void verify(bool check_dictionary = true) { verify_on(tty, check_dictionary); }

#ifndef PRODUCT
  bool verify_vtable_index(int index);
  bool verify_itable_index(int index);
#endif

  virtual void oop_verify_on(oop obj, outputStream* st);

  static bool is_null(narrowKlass obj);
  static bool is_null(Klass* obj);

  // klass encoding for klass pointer in objects.
  static narrowKlass encode_klass_not_null(Klass* v);
  static narrowKlass encode_klass(Klass* v);

  static Klass* decode_klass_not_null(narrowKlass v);
  static Klass* decode_klass(narrowKlass v);

 private:
  // barriers used by klass_oop_store
  void klass_update_barrier_set(oop v);
  void klass_update_barrier_set_pre(void* p, oop v);
};

#endif // SHARE_VM_OOPS_KLASS_HPP
