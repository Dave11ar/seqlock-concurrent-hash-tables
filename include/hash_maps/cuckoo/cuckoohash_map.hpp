#pragma once

#include <cassert>
#include <vector>
#include <optional>

#include <seqlock.hpp>
#include <lock_container.hpp>

#include "cuckoo_bucket_container.hpp"
#include "cuckoohash_util.hpp"

namespace seqlock_lib::cuckoo {

/**
 * A concurrent hash table
 *
 * @tparam Key type of keys in the table
 * @tparam T type of values in the table
 * @tparam Hash type of hash functor
 * @tparam KeyEqual type of equality comparison functor
 * @tparam Allocator type of allocator. We suggest using an aligned allocator,
 * because the table relies on types that are over-aligned to optimize
 * concurrent cache usage.
 * @tparam SLOT_PER_BUCKET number of slots for each bucket in the table
 */
template <class Key, class T, class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>,
          class Allocator = std::allocator<std::pair<const Key, T>>,
          std::size_t SLOT_PER_BUCKET = DEFAULT_SLOT_PER_BUCKET>
class cuckoohash_map {
private:
  // Type of the partial key
  using partial_t = uint8_t;

  // The type of the buckets container
  using buckets_t =
      cuckoo_bucket_container<Key, T, Allocator, partial_t, SLOT_PER_BUCKET>;

public:
  // The type of the bucket
  using bucket = typename buckets_t::typed_bucket;

  /** @name Type Declarations */
  /**@{*/
  using key_type = typename buckets_t::key_type;
  using mapped_type = typename buckets_t::mapped_type;

  /**
   * This type is defined as an @c std::pair. Note that table behavior is
   * undefined if a user-defined specialization of @c std::pair<Key, T> or @c
   * std::pair<const Key, T> exists.
   */
  using value_type = typename buckets_t::value_type;
  using size_type = typename buckets_t::size_type;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using allocator_type = typename buckets_t::allocator_type;
  using reference = typename buckets_t::reference;
  using const_reference = typename buckets_t::const_reference;
  using pointer = typename buckets_t::pointer;
  using const_pointer = typename buckets_t::const_pointer;

  class locked_table;
  /**@}*/

  /** @name Table Parameters */
  /**@{*/

  /**
   * The number of slots per hash bucket
   */
  static constexpr uint16_t slot_per_bucket() { return SLOT_PER_BUCKET; }

  /**@}*/

  /** @name Constructors, Destructors, and Assignment */
  /**@{*/

  /**
   * Creates a new cuckohash_map instance
   *
   * @param n the number of elements to reserve space for initially
   * @param hf hash function instance to use
   * @param equal equality function instance to use
   * @param alloc allocator instance to use
   */
  cuckoohash_map(size_type n = DEFAULT_SIZE, const Hash &hf = Hash(),
                 const KeyEqual &equal = KeyEqual(),
                 const Allocator &alloc = Allocator())
      : hash_fn_(hf), eq_fn_(equal),
        buckets_(reserve_calc(n), alloc),
        locks_(std::min(reserve_calc(n), kMaxNumLocksPow), alloc),
        minimum_load_factor_(DEFAULT_MINIMUM_LOAD_FACTOR),
        maximum_hashpower_(NO_MAXIMUM_HASHPOWER),
        max_num_worker_threads_(0) {}

  /**
   * Constructs the map with the contents of the range @c [first, last].  If
   * multiple elements in the range have equivalent keys, it is unspecified
   * which element is inserted.
   *
   * @param first the beginning of the range to copy from
   * @param last the end of the range to copy from
   * @param n the number of elements to reserve space for initially
   * @param hf hash function instance to use
   * @param equal equality function instance to use
   * @param alloc allocator instance to use
   */
  template <typename InputIt>
  cuckoohash_map(InputIt first, InputIt last,
                 size_type n = DEFAULT_SIZE, const Hash &hf = Hash(),
                 const KeyEqual &equal = KeyEqual(),
                 const Allocator &alloc = Allocator())
      : cuckoohash_map(n, hf, equal, alloc) {
    for (; first != last; ++first) {
      insert(first->first, first->second);
    }
  }

  /**
   * Copy constructor. If @p other is being modified concurrently, behavior is
   * unspecified.
   *
   * @param other the map being copied
   */
  cuckoohash_map(const cuckoohash_map &other) = default;

  /**
   * Copy constructor with separate allocator. If @p other is being modified
   * concurrently, behavior is unspecified.
   *
   * @param other the map being copied
   * @param alloc the allocator instance to use with the map
   */
  cuckoohash_map(const cuckoohash_map &other, const Allocator &alloc)
      : hash_fn_(other.hash_fn_), eq_fn_(other.eq_fn_),
        buckets_(other.buckets_, alloc),
        locks_(other.locks_, alloc),
        minimum_load_factor_(other.minimum_load_factor_),
        maximum_hashpower_(other.maximum_hashpower_),
        max_num_worker_threads_(other.max_num_worker_threads_) {}

  /**
   * Move constructor. If @p other is being modified concurrently, behavior is
   * unspecified.
   *
   * @param other the map being moved
   */
  cuckoohash_map(cuckoohash_map &&other) = default;

  /**
   * Move constructor with separate allocator. If the map being moved is being
   * modified concurrently, behavior is unspecified.
   *
   * @param other the map being moved
   * @param alloc the allocator instance to use with the map
   */
  cuckoohash_map(cuckoohash_map &&other, const Allocator &alloc)
      : hash_fn_(std::move(other.hash_fn_)), eq_fn_(std::move(other.eq_fn_)),
        buckets_(std::move(other.buckets_), alloc),
        locks_(std::move(other.locks_), alloc),
        minimum_load_factor_(other.minimum_load_factor_),
        maximum_hashpower_(other.maximum_hashpower_),
        max_num_worker_threads_(other.max_num_worker_threads_) {}

  /**
   * Constructs the map with the contents of initializer list @c init.
   *
   * @param init initializer list to initialize the elements of the map with
   * @param n the number of elements to reserve space for initially
   * @param hf hash function instance to use
   * @param equal equality function instance to use
   * @param alloc allocator instance to use
   */
  cuckoohash_map(std::initializer_list<value_type> init,
                 size_type n = DEFAULT_SIZE, const Hash &hf = Hash(),
                 const KeyEqual &equal = KeyEqual(),
                 const Allocator &alloc = Allocator())
      : cuckoohash_map(init.begin(), init.end(), n, hf, equal, alloc) {}

  /**
   * Exchanges the contents of the map with those of @p other
   *
   * @param other the map to exchange contents with
   */
  virtual void swap(cuckoohash_map &other) noexcept {
    std::swap(hash_fn_, other.hash_fn_);
    std::swap(eq_fn_, other.eq_fn_);
    buckets_.swap(other.buckets_);
    locks_.swap(other.locks_);
    other.minimum_load_factor_.store(
        minimum_load_factor_.exchange(other.minimum_load_factor(),
                                      std::memory_order_release),
        std::memory_order_release);
    other.maximum_hashpower_.store(
        maximum_hashpower_.exchange(other.maximum_hashpower(),
                                    std::memory_order_release),
        std::memory_order_release);
  }

  /**
   * Copy assignment operator. If @p other is being modified concurrently,
   * behavior is unspecified.
   *
   * @param other the map to assign from
   * @return @c *this
   */
  cuckoohash_map &operator=(const cuckoohash_map &other) = default;

  /**
   * Move assignment operator. If @p other is being modified concurrently,
   * behavior is unspecified.
   *
   * @param other the map to assign from
   * @return @c *this
   */
  cuckoohash_map &operator=(cuckoohash_map &&other) = default;

  /**
   * Initializer list assignment operator
   *
   * @param ilist an initializer list to assign from
   * @return @c *this
   */
  cuckoohash_map &operator=(std::initializer_list<value_type> ilist) {
    buckets_.clear();
    for (seqlock& lock : locks_) {
      lock.elem_counter() = 0;
    }
    for (const auto &item : ilist) {
      insert(item.first, item.second);
    }
    return *this;
  }

  /**@}*/

  /** @name Table Details
   *
   * Methods for getting information about the table. Methods that query
   * changing properties of the table are not synchronized with concurrent
   * operations, and may return out-of-date information if the table is being
   * concurrently modified. They will also continue to work after the container
   * has been moved.
   *
   */
  /**@{*/

  /**
   * Returns the function that hashes the keys
   *
   * @return the hash function
   */
  hasher hash_function() const { return hash_fn_; }

  /**
   * Returns the function that compares keys for equality
   *
   * @return the key comparison function
   */
  key_equal key_eq() const { return eq_fn_; }

  /**
   * Returns the allocator associated with the map
   *
   * @return the associated allocator
   */
  allocator_type get_allocator() const { return buckets_.get_allocator(); }

  /**
   * Returns the hashpower of the table, which is log<SUB>2</SUB>(@ref
   * bucket_count()).
   *
   * @return the hashpower
   */
  size_type hashpower() const { return buckets_.hashpower(); }

  /**
   * Returns the number of buckets in the table.
   *
   * @return the bucket count
   */
  size_type bucket_count() const { return buckets_.size(); }

  /**
   * Returns whether the table is empty or not.
   *
   * @return true if the table is empty, false otherwise
   */
  bool empty() const { return size() == 0; }

  /**
   * Returns the number of elements in the table.
   *
   * @return number of elements in the table
   */
  size_type size() const {
    counter_type s = 0;
    for (seqlock& lock : locks_) {
      s += lock.elem_counter();
    }
    assert(s >= 0);
    return static_cast<size_type>(s);
  }

  /** Returns the current capacity of the table, that is, @ref bucket_count()
   * &times; @ref slot_per_bucket().
   *
   * @return capacity of table
   */
  size_type capacity() const { return bucket_count() * slot_per_bucket(); }

  /**
   * Returns the percentage the table is filled, that is, @ref size() &divide;
   * @ref capacity().
   *
   * @return load factor of the table
   */
  double load_factor() const {
    return static_cast<double>(size()) / static_cast<double>(capacity());
  }

  /**
   * Sets the minimum load factor allowed for automatic expansions. If an
   * expansion is needed when the load factor of the table is lower than this
   * threshold, @ref load_factor_too_low is thrown. It will not be
   * thrown for an explicitly-triggered expansion.
   *
   * @param mlf the load factor to set the minimum to
   * @throw std::invalid_argument if the given load factor is less than 0.0
   * or greater than 1.0
   */
  void minimum_load_factor(const double mlf) {
    if (mlf < 0.0) {
      throw std::invalid_argument("load factor " + std::to_string(mlf) +
                                  " cannot be "
                                  "less than 0");
    } else if (mlf > 1.0) {
      throw std::invalid_argument("load factor " + std::to_string(mlf) +
                                  " cannot be "
                                  "greater than 1");
    }
    minimum_load_factor_.store(mlf, std::memory_order_release);
  }

  /**
   * Returns the minimum load factor of the table
   *
   * @return the minimum load factor
   */
  double minimum_load_factor() const {
    return minimum_load_factor_.load(std::memory_order_acquire);
  }

  /**
   * Sets the maximum hashpower the table can be. If set to @ref
   * NO_MAXIMUM_HASHPOWER, there will be no limit on the hashpower.
   * Otherwise, the table will not be able to expand beyond the given
   * hashpower, either by an explicit or an automatic expansion.
   *
   * @param mhp the hashpower to set the maximum to
   * @throw std::invalid_argument if the current hashpower exceeds the limit
   */
  void maximum_hashpower(size_type mhp) {
    if (hashpower() > mhp) {
      throw std::invalid_argument("maximum hashpower " + std::to_string(mhp) +
                                  " is less than current hashpower");
    }
    maximum_hashpower_.store(mhp, std::memory_order_release);
  }

  /**
   * Returns the maximum hashpower of the table
   *
   * @return the maximum hashpower
   */
  size_type maximum_hashpower() const {
    return maximum_hashpower_.load(std::memory_order_acquire);
  }


  /**
   * Set the maximum number of extra worker threads the table can spawn when
   * doing large batch operations. Currently batch operations occur in the
   * following scenarios.
   *   - Any resizing operation which invokes cuckoo_fast_double. This
   *   includes any explicit rehash/resize operation, or any general resize if
   *   the data is not nothrow-move-constructible.
   *   - Creating a locked_table or resizing within a locked_table.
   *
   * @param num_threads the number of extra threads
   */
  void max_num_worker_threads(size_type extra_threads) {
    max_num_worker_threads_.store(extra_threads, std::memory_order_release);
  }

  /**
   * Returns the maximum number of extra worker threads.
   */
  size_type max_num_worker_threads() const {
    return max_num_worker_threads_.load(std::memory_order_acquire);
  }

  /**@}*/

  /** @name Table Operations
   *
   * These are operations that affect the data in the table. They are safe to
   * call concurrently with each other.
   *
   */
  /**@{*/

  /**
   * Searches the table for @p key, and invokes @p fn on the value.
   *
   * @tparam K type of the key. This can be any type comparable with @c key_type
   * @tparam F type of the functor. It should implement the method
   * <tt>void operator()(const mapped_type&)</tt>.
   * @param key the key to search for
   * @param fn the functor to invoke if the element is found
   * @return true if the key was found and functor invoked, false otherwise
   */
  template <typename K, typename F> bool find_fn(const K &key, F fn) const {
    std::optional<mapped_type> value_opt = read_value(key);

    if (value_opt.has_value()) {
      fn(std::move(value_opt.value()));
      return true;
    } else {
      return false;
    }
  }

  /**
   * Searches the table for @p key, and invokes @p fn on the value. @p fn is
   * allow to modify the contents of the value if found.
   *
   * @tparam K type of the key. This can be any type comparable with @c key_type
   * @tparam F type of the functor. It should implement the method
   * <tt>void operator()(mapped_type&)</tt>.
   * @param key the key to search for
   * @param fn the functor to invoke if the element is found
   * @return true if the key was found and functor invoked, false otherwise
   */
  template <typename K, typename F> bool update_fn(const K &key, F fn) {
    const hash_value hv = hashed_key(key);
    TwoBuckets b = snapshot_and_lock_two<normal_mode>(hv);
    const table_position pos = cuckoo_find(key, hv.partial, b.i1, b.i2);
    if (pos.status == ok) {
      fn(pos.bucket_->mapped(pos.slot));
      return true;
    } else {
      b.unlock(false);
      return false;
    }
  }

  /**
   * Searches for @p key in the table, and invokes @p fn on the value if the
   * key is found. The functor can mutate the value, and should return @c true
   * in order to erase the element, and @c false otherwise.
   *
   * @tparam K type of the key
   * @tparam F type of the functor. It should implement the method
   * <tt>bool operator()(mapped_type&)</tt>.
   * @param key the key to possibly erase from the table
   * @param fn the functor to invoke if the element is found
   * @return true if @p key was found and @p fn invoked, false otherwise
   */
  template <typename K, typename F> bool erase_fn(const K &key, F fn) {
    const hash_value hv = hashed_key(key);
    TwoBuckets b = snapshot_and_lock_two<normal_mode>(hv);
    const table_position pos = cuckoo_find(key, hv.partial, b.i1, b.i2);
    if (pos.status == ok) {
      if (fn(pos.bucket_->mapped(pos.slot))) {
        del_from_bucket(*b.lock1, *pos.bucket_, pos.slot);
      } else {
        b.unlock(false);
      }
      return true;
    } else {
      b.unlock(false);
      return false;
    }
  }

  /**
   * Searches for @p key in the table. If the key is found, then @p fn is
   * called on the existing value, and nothing happens to the passed-in key and
   * values. The functor can mutate the value, and should return @c true in
   * order to erase the element, and @c false otherwise. If the key is not
   * found and must be inserted, the pair will be constructed by forwarding the
   * given key and values. If there is no room left in the table, it will be
   * automatically expanded. Expansion may throw exceptions.
   *
   * @tparam K type of the key
   * @tparam F type of the functor. It should implement the method
   * <tt>bool operator()(mapped_type&)</tt>.
   * @tparam Args list of types for the value constructor arguments
   * @param key the key to insert into the table
   * @param fn the functor to invoke if the element is found. If your @p fn
   * needs more data that just the value being modified, consider implementing
   * it as a lambda with captured arguments.
   * @param val a list of constructor arguments with which to create the value
   * @return true if a new key was inserted, false if the key was already in
   * the table
   */
  template <typename K, typename F, typename ...Args>
  bool uprase_fn(K&& key, F fn, Args&& ...val) {
    hash_value hv = hashed_key(key);
    TwoBuckets b = snapshot_and_lock_two<normal_mode>(hv);
    table_position pos = cuckoo_insert_loop<normal_mode>(hv, b, key);
    if (pos.status == ok) {
      add_to_bucket(*b.lock1, 
                    *pos.bucket_, 
                    pos.slot, 
                    hv.partial,
                    std::forward<K>(key),
                    std::forward<Args>(val)...);
    } else {
      if (fn(pos.bucket_->mapped(pos.slot))) {
        del_from_bucket(*b.lock1, *pos.bucket_, pos.slot);
      } else {
        b.unlock(false);
      }
    }
    return pos.status == ok;
  }

  /**
   * Equivalent to calling @ref uprase_fn with a functor that modifies the
   * given value and always returns false (meaning the element is not removed).
   * The passed-in functor must implement the method <tt>void
   * operator()(mapped_type&)</tt>.
   */
  template <typename K, typename F, typename... Args>
  bool upsert(K &&key, F fn, Args &&... val) {
    return uprase_fn(std::forward<K>(key),
                     [&fn](mapped_type &v) {
                       fn(v);
                       return false;
                     },
                     std::forward<Args>(val)...);
  }

  /**
   * Copies the value associated with @p key into @p val. Equivalent to
   * calling @ref find_fn with a functor that copies the value into @p val. @c
   * mapped_type must be @c CopyAssignable.
   */
  template <typename K> bool find(const K &key, mapped_type &val) const {
    return find_fn(key, [&val](const mapped_type &v) mutable { val = v; });
  }

  /** Searches the table for @p key, and returns the associated value it
   * finds. @c mapped_type must be @c CopyConstructible.
   *
   * @tparam K type of the key
   * @param key the key to search for
   * @return the value associated with the given key
   * @std::out_of_range if the key is not found
   */
  template <typename K> mapped_type find(const K &key) const {
    const auto value_opt = read_value(key);

    if (value_opt.has_value()) {
      return value_opt.value();
    } else {
      throw std::out_of_range("key not found in table");
    }
  }

  /**
   * Returns whether or not @p key is in the table. Equivalent to @ref
   * find_fn with a functor that does nothing.
   */
  template <typename K> bool contains(const K &key) const {
    return find_fn(key, [](const mapped_type &) {});
  }

  /**
   * Updates the value associated with @p key to @p val. Equivalent to
   * calling @ref update_fn with a functor that assigns the existing mapped
   * value to @p val. @c mapped_type must be @c MoveAssignable or @c
   * CopyAssignable.
   */
  template <typename K, typename V> bool update(const K &key, V &&val) {
    return update_fn(key, [&val](mapped_type &v) { v = std::forward<V>(val); });
  }

  /**
   * Inserts the key-value pair into the table. Equivalent to calling @ref
   * upsert with a functor that does nothing.
   */
  template <typename K, typename... Args> bool insert(K &&key, Args &&... val) {
    return upsert(std::forward<K>(key), [](mapped_type &) {},
                  std::forward<Args>(val)...);
  }

  /**
   * Inserts the key-value pair into the table. If the key is already in the
   * table, assigns the existing mapped value to @p val. Equivalent to
   * calling @ref upsert with a functor that assigns the mapped value to @p
   * val.
   */
  template <typename K, typename V> bool insert_or_assign(K &&key, V &&val) {
    return upsert(std::forward<K>(key), [&val](mapped_type &m) { m = val; },
                  std::forward<V>(val));
  }

  /**
   * Erases the key from the table. Equivalent to calling @ref erase_fn with a
   * functor that just returns true.
   */
  template <typename K> bool erase(const K &key) {
    return erase_fn(key, [](mapped_type &) { return true; });
  }

  /**
   * Resizes the table to the given hashpower. If this hashpower is not larger
   * than the current hashpower, then it decreases the hashpower to the
   * maximum of the specified value and the smallest hashpower that can hold
   * all the elements currently in the table.
   *
   * @param n the hashpower to set for the table
   * @return true if the table changed size, false otherwise
   */
  bool rehash(size_type n) { return cuckoo_rehash<normal_mode>(n); }
  bool rehash_concurrent(size_type n) { return cuckoo_rehash_concurrent<normal_mode>(n); }
  /**
   * Reserve enough space in the table for the given number of elements. If
   * the table can already hold that many elements, the function will shrink
   * the table to the smallest hashpower that can hold the maximum of the
   * specified amount and the current table size.
   *
   * @param n the number of elements to reserve space for
   * @return true if the size of the table changed, false otherwise
   */
  bool reserve(size_type n) { return cuckoo_reserve<normal_mode>(n); }
  bool reserve_concurrent(size_type n) { return cuckoo_reserve_concurrent<normal_mode>(n); }

  /**
   * Removes all elements in the table, calling their destructors.
   */
  void clear() {
    auto all_locks_manager = lock_all(normal_mode());
    cuckoo_clear();
  }

  /**
   * Construct a @ref locked_table object that owns all the locks in the
   * table.
   *
   * @return a \ref locked_table instance
   */
  locked_table lock_table() { return locked_table(*this); }

  /**@}*/

private:
  // Hashing types and functions

  // true if the key is small and simple, which means using partial keys for
  // lookup would probably slow us down
  static constexpr bool is_simple() {
    return std::is_standard_layout<key_type>::value &&
           std::is_trivial<key_type>::value &&
           sizeof(key_type) <= 8;
  }

  // Whether or not the data is nothrow-move-constructible.
  static constexpr bool is_data_nothrow_move_constructible() {
    return std::is_nothrow_move_constructible<key_type>::value &&
           std::is_nothrow_move_constructible<mapped_type>::value;
  }

  // Contains a hash and partial for a given key. The partial key is used for
  // partial-key cuckoohashing, and for finding the alternate bucket of that a
  // key hashes to.
  struct hash_value {
    size_type hash;
    partial_t partial;
  };

  template <typename K> hash_value hashed_key(const K &key) const {
    const size_type hash = hash_function()(key);
    return {hash, partial_key(hash)};
  }

  template <typename K> size_type hashed_key_only_hash(const K &key) const {
    return hash_function()(key);
  }

  // hashsize returns the number of buckets corresponding to a given
  // hashpower.
  static inline size_type hashsize(const size_type hp) {
    return size_type(1) << hp;
  }

  // hashmask returns the bitmask for the buckets array corresponding to a
  // given hashpower.
  static inline size_type hashmask(const size_type hp) {
    return hashsize(hp) - 1;
  }

  // The partial key must only depend on the hash value. It cannot change with
  // the hashpower, because, in order for `cuckoo_fast_double` to work
  // properly, the alt_index must only grow by one bit at the top each time we
  // expand the table.
  static partial_t partial_key(const size_type hash) {
    const uint64_t hash_64bit = hash;
    const uint32_t hash_32bit = (static_cast<uint32_t>(hash_64bit) ^
                                 static_cast<uint32_t>(hash_64bit >> 32));
    const uint16_t hash_16bit = (static_cast<uint16_t>(hash_32bit) ^
                                 static_cast<uint16_t>(hash_32bit >> 16));
    const uint8_t hash_8bit = (static_cast<uint8_t>(hash_16bit) ^
                               static_cast<uint8_t>(hash_16bit >> 8));
    return hash_8bit;
  }

  // index_hash returns the first possible bucket that the given hashed key
  // could be.
  static inline size_type index_hash(const size_type hp, const size_type hv) {
    return hv & hashmask(hp);
  }

  // alt_index returns the other possible bucket that the given hashed key
  // could be. It takes the first possible bucket as a parameter. Note that
  // this function will return the first possible bucket if index is the
  // second possible bucket, so alt_index(ti, partial, alt_index(ti, partial,
  // index_hash(ti, hv))) == index_hash(ti, hv).
  static inline size_type alt_index(const size_type hp, const partial_t partial,
                                    const size_type index) {
    // ensure tag is nonzero for the multiply. 0xc6a4a7935bd1e995 is the
    // hash constant from 64-bit MurmurHash2
    const size_type nonzero_tag = static_cast<size_type>(partial) + 1;
    return (index ^ (nonzero_tag * 0xc6a4a7935bd1e995)) & hashmask(hp);
  }

  // Locking types

  // Counter type
  using counter_type = int64_t;


  template <typename U>
  using rebind_alloc =
      typename std::allocator_traits<allocator_type>::template rebind_alloc<U>;

  using locks_t = lock_container<seqlock, Allocator>;

  // Classes for managing locked buckets. By storing and moving around sets of
  // locked buckets in these classes, we can ensure that they are unlocked
  // properly.

  using LockManager = std::unique_ptr<seqlock, LockDeleter<seqlock>>;

  // Each of the locking methods can operate in two modes: locked_table_mode
  // and normal_mode. When we're in locked_table_mode, we assume the caller has
  // already taken all locks on the buckets. We also require that all data is
  // rehashed immediately, so that the caller never has to look through any
  // locks. In normal_mode, we actually do take locks, and can rehash lazily.
  using locked_table_mode = std::integral_constant<bool, true>;
  using normal_mode = std::integral_constant<bool, false>;

  class TwoBuckets {
  public:
    TwoBuckets() {}
    TwoBuckets(size_type i1_, size_type i2_, locked_table_mode)
        : i1(i1_), i2(i2_) {}
    TwoBuckets(seqlock* lock1_, seqlock* lock2_, size_type i1_, size_type i2_, normal_mode)
        : i1(i1_), i2(i2_),
          lock1(lock1_), lock2(lock1_ == lock2_ ? nullptr : lock2_) {}

    void unlock(bool is_modified = true) noexcept {
      if (is_modified) {
        lock1.reset();

        if (i1 != i2) {
          lock2.reset();
        }
      } else {
        if (lock1 != nullptr) {
          lock1->unlock_no_modified();
          (void)lock1.release();
        }

        if (i1 != i2 && lock2 != nullptr) {
          lock2->unlock_no_modified();
          (void)lock2.release();
        }
      }
    }

    LockManager lock1;
    LockManager lock2;
    size_type i1, i2;
  };

  struct AllUnlocker {
    void operator()(cuckoohash_map *map) const {
      for (seqlock &lock : map->locks_) {
        lock.unlock();
      }
    }
  };

  using AllLocksManager = std::unique_ptr<cuckoohash_map, AllUnlocker>;

  // After taking a lock on the table for the given bucket, this function will
  // check the hashpower to make sure it is the same as what it was before the
  // lock was taken. If it isn't unlock the bucket and throw a
  // hashpower_changed exception.
  inline void check_hashpower(size_type hp, seqlock &lock) const {
    if (hashpower() != hp) {
      lock.unlock();
      LIBCUCKOO_DBG("%s", "hashpower changed\n");
      throw hashpower_changed();
    }
  }


  void migrate_lock(size_type l) const {
    assert(is_data_nothrow_move_constructible());
    assert(locks_.size() == kMaxNumLocks);

    const int32_t hp = hashpower() - 1;
    for (size_type bucket_ind = l; bucket_ind < (buckets_.size() >> 1); bucket_ind += kMaxNumLocks) {
      move_bucket(hp, bucket_ind, buckets_[bucket_ind], buckets_[bucket_ind + hashsize(hashpower() - 1)], 
      [this, bucket_ind](size_type old_bucket_slot){
        buckets_.eraseKV(bucket_ind, old_bucket_slot);
      });
    }
  }

  // If necessary, rehashes the buckets corresponding to the given lock index,
  // and sets the is_migrated flag to true. We should only ever do migrations
  // if the data is nothrow move constructible, so this function is noexcept.
  template <typename TABLE_MODE>
  seqlock* lock_and_rehash(size_type l) const {
    seqlock* lock = &locks_[l];

    seqlock_epoch_t lock_value;
    if constexpr (std::is_same<TABLE_MODE, normal_mode>::value) {
      lock_value = lock->lock();
    } else {
      lock_value = lock->get_epoch();
    }

    if (!seqlock::is_migrated(lock_value)) {
      migrate_lock(l);
      lock->set_migrated(true);
    }

    return lock;
  }

  // locks the given bucket index.
  //
  // throws hashpower_changed if it changed after taking the lock.
  LockManager lock_one(size_type, size_type, locked_table_mode) const {
    return LockManager();
  }

  LockManager lock_one(size_type hp, size_type i, normal_mode) const {
    seqlock* lock = lock_and_rehash<normal_mode>(lock_ind(i));
    check_hashpower(hp, *lock);
    return LockManager(lock);
  }

  // locks the two bucket indexes, always locking the earlier index first to
  // avoid deadlock. If the two indexes are the same, it just locks one.
  //
  // throws hashpower_changed if it changed after taking the lock.
  TwoBuckets lock_two(size_type, size_type i1, size_type i2,
                      locked_table_mode) const {
    return TwoBuckets(i1, i2, locked_table_mode());
  }
  TwoBuckets lock_two(size_type hp, size_type i1, size_type i2,
                      normal_mode) const {
    size_type l1 = lock_ind(i1);
    size_type l2 = lock_ind(i2);

    if (l2 < l1) {
      std::swap(l1, l2);
    }

    seqlock* lock1 = lock_and_rehash<normal_mode>(l1);
    seqlock* lock2 = nullptr;

    check_hashpower(hp,*lock1);
    if (l1 != l2) {
      lock2 = lock_and_rehash<normal_mode>(l2);
    }


    return TwoBuckets(lock1, lock2, i1, i2, normal_mode());
  }

  // lock_three locks the three bucket indexes in numerical order, returning
  // the containers as a two (i1 and i2) and a one (i3). The one will not be
  // active if i3 shares a lock index with i1 or i2.
  //
  // throws hashpower_changed if it changed after taking the lock.
  std::pair<TwoBuckets, LockManager> lock_three(size_type, std::array<size_type, 3> i,
                                                locked_table_mode) const {
    return std::make_pair(TwoBuckets(i[0], i[1], locked_table_mode()),
                          LockManager());
  }

  std::pair<TwoBuckets, LockManager> lock_three(
      size_type hp, std::array<size_type, 3> i, normal_mode) const {
    std::array<size_type, 3> l{{lock_ind(i[0]), lock_ind(i[1]), lock_ind(i[2])}};
    std::array<uint8_t, 3> l2i{{0, 1, 2}};
    // Lock in order.
    if (l[2] < l[1]) {
      std::swap(l[2], l[1]);
      std::swap(l2i[2], l2i[1]);
    }
    if (l[2] < l[0]) {
      std::swap(l[2], l[0]);
      std::swap(l2i[2], l2i[0]);
    }
    if (l[1] < l[0]) {
      std::swap(l[1], l[0]);
      std::swap(l2i[1], l2i[0]);
    }

    std::array<seqlock*, 3> cur_locks{{
      lock_and_rehash<normal_mode>(l[0]), 
      nullptr, 
      nullptr
    }};

    check_hashpower(hp, *cur_locks[0]);
    if (l[1] != l[0]) {
      cur_locks[1] = lock_and_rehash<normal_mode>(l[1]);
    }
    if (l[2] != l[1]) {
      cur_locks[2] = lock_and_rehash<normal_mode>(l[2]);
    }

    for (uint8_t j = 0; j < 3; ++j) {
      l[l2i[j]] = j;
    }

    return std::make_pair(TwoBuckets(cur_locks[l[0]], cur_locks[l[1]], i[0], i[1], normal_mode()),
                          LockManager(cur_locks[l[2]] == cur_locks[l[0]] ||
                                      cur_locks[l[2]] == cur_locks[l[1]]
                                          ? nullptr
                                          : cur_locks[l[2]]));
  }

  // snapshot_and_lock_two loads locks the buckets associated with the given
  // hash value, making sure the hashpower doesn't change before the locks are
  // taken. Thus it ensures that the buckets and locks corresponding to the
  // hash value will stay correct as long as the locks are held. It returns
  // the bucket indices associated with the hash value and the current
  // hashpower.
  template <typename TABLE_MODE>
  TwoBuckets snapshot_and_lock_two(const hash_value &hv) const {
    while (true) {
      // Keep the current hashpower and locks we're using to compute the buckets
      const size_type hp = hashpower();
      const size_type i1 = index_hash(hp, hv.hash);
      const size_type i2 = alt_index(hp, hv.partial, i1);
      try {
        return lock_two(hp, i1, i2, TABLE_MODE());
      } catch (hashpower_changed &) {
        // The hashpower changed while taking the locks. Try again.
        continue;
      }
    }
  }

  // lock_all takes all the locks, and returns a deleter object that releases
  // the locks upon destruction. It does NOT perform any hashpower checks, or
  // rehash any un-migrated buckets.
  //
  // Note that after taking all the locks, it is okay to resize the buckets_
  // container, since no other threads should be accessing the buckets.
  AllLocksManager lock_all(locked_table_mode) {
    return AllLocksManager();
  }

  AllLocksManager lock_all(normal_mode) {
    auto it = locks_.begin();
    it->lock();
    ++it;
    for (; it != locks_.end(); ++it) {
      it->lock();
    }

    // Once we have taken all the locks of the "current" container, nobody
    // else can do locking operations on the table.
    return AllLocksManager(this, AllUnlocker());
  }

  // lock_ind converts an index into buckets to an index into locks.
  static inline size_type lock_ind(const size_type bucket_ind) {
    return bucket_ind & (kMaxNumLocks - 1);
  }

  // Data storage types and functions

  // Status codes for internal functions

  enum cuckoo_status {
    ok,
    failure,
    failure_key_not_found,
    failure_key_duplicated,
    failure_table_full,
    failure_under_expansion,
  };

  // A composite type for functions that need to return a table position, and
  // a status code.
  struct table_position {
    bucket* bucket_;
    size_type index;
    size_type slot;
    cuckoo_status status;
  };

  // should retry if returns seqlock::lock_bit
  seqlock* read_and_rehash(seqlock_epoch_t& epoch, size_type l) const {
    seqlock* lock = &locks_[l];
    epoch = lock->get_epoch();

    if (seqlock::is_locked(epoch)) {
      return nullptr;
    }
    if (!seqlock::is_migrated(epoch)) {
      epoch = lock->lock();
      if (!seqlock::is_migrated(epoch)) {
        migrate_lock(l);
        lock->set_migrated(true);
      }
      lock->unlock();

      return nullptr;
    }

    return lock;
  }

  template <typename K>
  std::optional<mapped_type> read_value(const K &key) const {
    const hash_value hv = hashed_key(key);

    while (true) {
      const size_type hp = hashpower();
      const size_type i1 = index_hash(hp, hv.hash);
      const size_type i2 = alt_index(hp, hv.partial, i1);
      const size_type l1 = lock_ind(i1);
      const size_type l2 = lock_ind(i2);

      seqlock_epoch_t epoch1;
      seqlock* lock1 = read_and_rehash(epoch1, l1);

      if (lock1 == nullptr || hp != hashpower()) {
        continue;
      }

      seqlock_epoch_t epoch2;
      seqlock* lock2;

      if (l1 != l2) {
        lock2 = read_and_rehash(epoch2, l2);

        if (lock2 == nullptr) {
          continue;
        }
      }

      const table_position pos = cuckoo_find(key, hv.partial, i1, i2);

      std::optional<mapped_type> value_opt;
      if (pos.status == ok) {
        value_opt = pos.bucket_->mapped(pos.slot);
      }

#ifdef __x86_64__
      asm volatile ("" ::: "memory");
#elif defined(__aarch64__)
      asm volatile ("DMB ISHLD" ::: "memory");
#else
      #error
#endif

      if (epoch1 == lock1->get_epoch() && (l1 == l2 || epoch2 == lock2->get_epoch())) {
        return value_opt;
      }
    }
  }

  // Searching types and functions

  // cuckoo_find searches the table for the given key, returning the position
  // of the element found, or a failure status code if the key wasn't found.
  // It expects the locks to be taken and released outside the function.
  template <typename K>
  table_position cuckoo_find(const K &key, const partial_t partial,
                             const size_type i1, const size_type i2) const {
    bucket* bucket1 = &buckets_[i1];
    int slot = try_read_from_bucket(*bucket1, partial, key);
    if (slot != -1) {
      return table_position{bucket1, i1, static_cast<size_type>(slot), ok};
    }

    bucket* bucket2 = &buckets_[i2];
    slot = try_read_from_bucket(*bucket2, partial, key);
    if (slot != -1) {
      return table_position{bucket2, i2, static_cast<size_type>(slot), ok};
    }
    return table_position{nullptr, 0, 0, failure_key_not_found};
  }

  // try_read_from_bucket will search the bucket for the given key and return
  // the index of the slot if found, or -1 if not found.
  template <typename K>
  int try_read_from_bucket(const bucket &b, const partial_t partial,
                           const K &key) const {
    // Silence a warning from MSVC about partial being unused if is_simple.
    (void)partial;
    for (int i = 0; i < static_cast<int>(slot_per_bucket()); ++i) {
      if (!b.occupied(i) || (!is_simple() && partial != b.partial(i))) {
        continue;
      } else if (key_eq()(b.key(i), key)) {
        return i;
      }
    }
    return -1;
  }

  // Insertion types and function

  /**
   * Runs cuckoo_insert in a loop until it succeeds in insert and upsert, so
   * we pulled out the loop to avoid duplicating logic.
   *
   * @param hv the hash value of the key
   * @param b bucket locks
   * @param key the key to insert
   * @return table_position of the location to insert the new element, or the
   * site of the duplicate element with a status code if there was a duplicate.
   * In either case, the locks will still be held after the function ends.
   * @throw load_factor_too_low if expansion is necessary, but the
   * load factor of the table is below the threshold
   */
  template <typename TABLE_MODE, typename K>
  table_position cuckoo_insert_loop(hash_value hv, TwoBuckets& b, K &key) {
    table_position pos;
    while (true) {
      const size_type hp = hashpower();
      pos = cuckoo_insert<TABLE_MODE>(hv, b, key);
      switch (pos.status) {
      case ok:
      case failure_key_duplicated:
        return pos;
      case failure_table_full:
        // Expand the table and try again, re-grabbing the locks
        cuckoo_fast_double<TABLE_MODE, automatic_resize>(hp);
        b = snapshot_and_lock_two<TABLE_MODE>(hv);
        break;
      case failure_under_expansion:
        // The table was under expansion while we were cuckooing. Re-grab the
        // locks and try again.
        b = snapshot_and_lock_two<TABLE_MODE>(hv);
        break;
      default:
        assert(false);
      }
    }
  }

  // cuckoo_insert tries to find an empty slot in either of the buckets to
  // insert the given key into, performing cuckoo hashing if necessary. It
  // expects the locks to be taken outside the function. Before inserting, it
  // checks that the key isn't already in the table. cuckoo hashing presents
  // multiple concurrency issues, which are explained in the function. The
  // following return states are possible:
  //
  // ok -- Found an empty slot, locks will be held on both buckets after the
  // function ends, and the position of the empty slot is returned
  //
  // failure_key_duplicated -- Found a duplicate key, locks will be held, and
  // the position of the duplicate key will be returned
  //
  // failure_under_expansion -- Failed due to a concurrent expansion
  // operation. Locks are released. No meaningful position is returned.
  //
  // failure_table_full -- Failed to find an empty slot for the table. Locks
  // are released. No meaningful position is returned.
  template <typename TABLE_MODE, typename K>
  table_position cuckoo_insert(const hash_value hv, TwoBuckets &b, K &key) {
    int res1, res2;
    bucket* bucket1 = &buckets_[b.i1];
    if (!try_find_insert_bucket(*bucket1, res1, hv.partial, key)) {
      return table_position{bucket1, b.i1, static_cast<size_type>(res1),
                            failure_key_duplicated};
    }
    bucket* bucket2 = &buckets_[b.i2];
    if (!try_find_insert_bucket(*bucket2, res2, hv.partial, key)) {
      return table_position{bucket2, b.i2, static_cast<size_type>(res2),
                            failure_key_duplicated};
    }
    if (res1 != -1) {
      return table_position{bucket1, b.i1, static_cast<size_type>(res1), ok};
    }
    if (res2 != -1) {
      return table_position{bucket2, b.i2, static_cast<size_type>(res2), ok};
    }

    // We are unlucky, so let's perform cuckoo hashing.
    size_type insert_bucket = 0;
    size_type insert_slot = 0;
    cuckoo_status st = run_cuckoo<TABLE_MODE>(b, insert_bucket, insert_slot);
    if (st == failure_under_expansion) {
      // The run_cuckoo operation operated on an old version of the table,
      // so we have to try again. We signal to the calling insert method
      // to try again by returning failure_under_expansion.
      return table_position{nullptr, 0, 0, failure_under_expansion};
    } else if (st == ok) {
      // assert(TABLE_MODE() == locked_table_mode() ||
      //        !get_current_locks()[lock_ind(b.i1)].try_lock());
      // assert(TABLE_MODE() == locked_table_mode() ||
      //        !get_current_locks()[lock_ind(b.i2)].try_lock());
      assert(!buckets_[insert_bucket].occupied(insert_slot));
      assert(insert_bucket == index_hash(hashpower(), hv.hash) ||
             insert_bucket == alt_index(hashpower(), hv.partial,
                                        index_hash(hashpower(), hv.hash)));
      // Since we unlocked the buckets during run_cuckoo, another insert
      // could have inserted the same key into either b.i1 or
      // b.i2, so we check for that before doing the insert.
      table_position pos = cuckoo_find(key, hv.partial, b.i1, b.i2);
      if (pos.status == ok) {
        pos.status = failure_key_duplicated;
        return pos;
      }
      return table_position{&buckets_[insert_bucket], insert_bucket, insert_slot, ok};
    }
    assert(st == failure);
    LIBCUCKOO_DBG("hash table is full (hashpower = %zu, hash_items = %zu,"
                  "load factor = %.2f), need to increase hashpower\n",
                  hashpower(), size(), load_factor());
    return table_position{nullptr, 0, 0, failure_table_full};
  }

  // add_to_bucket will insert the given key-value pair into the slot. The key
  // and value will be move-constructed into the table, so they are not valid
  // for use afterwards.
  template <typename K, typename... Args>
  void add_to_bucket(seqlock& lock_, bucket& bucket_, const size_type slot,
                     const partial_t partial, K &&key, Args &&... val) const {
    buckets_.setKV(bucket_, slot, partial, std::forward<K>(key),
                    std::forward<Args>(val)...);
    ++lock_.elem_counter();
  }

    template <typename K, typename... Args>
  void add_to_bucket(const size_type index, const size_type slot,
                     const partial_t partial, K&& key, Args&& ...val) const {
    buckets_.setKV(index, slot, partial, std::forward<K>(key),
                    std::forward<Args>(val)...);
    ++locks_[lock_ind(index)].elem_counter();
  }

  // try_find_insert_bucket will search the bucket for the given key, and for
  // an empty slot. If the key is found, we store the slot of the key in
  // `slot` and return false. If we find an empty slot, we store its position
  // in `slot` and return true. If no duplicate key is found and no empty slot
  // is found, we store -1 in `slot` and return true.
  template <typename K>
  bool try_find_insert_bucket(const bucket &b, int &slot,
                              const partial_t partial, const K &key) const {
    // Silence a warning from MSVC about partial being unused if is_simple.
    (void)partial;
    slot = -1;
    for (int i = 0; i < static_cast<int>(slot_per_bucket()); ++i) {
      if (b.occupied(i)) {
        if (!is_simple() && partial != b.partial(i)) {
          continue;
        }
        if (key_eq()(b.key(i), key)) {
          slot = i;
          return false;
        }
      } else {
        slot = i;
      }
    }
    return true;
  }

  // CuckooRecord holds one position in a cuckoo path. Since cuckoopath
  // elements only define a sequence of alternate hashings for different hash
  // values, we only need to keep track of the hash values being moved, rather
  // than the keys themselves.
  typedef struct {
    size_type bucket;
    size_type slot;
    hash_value hv;
  } CuckooRecord;

  // The maximum number of items in a cuckoo BFS path. It determines the
  // maximum number of slots we search when cuckooing.
  static constexpr uint8_t MAX_BFS_PATH_LEN = 5;

  // An array of CuckooRecords
  using CuckooRecords = std::array<CuckooRecord, MAX_BFS_PATH_LEN>;

  // run_cuckoo performs cuckoo hashing on the table in an attempt to free up
  // a slot on either of the insert buckets, which are assumed to be locked
  // before the start. On success, the bucket and slot that was freed up is
  // stored in insert_bucket and insert_slot. In order to perform the search
  // and the swaps, it has to release the locks, which can lead to certain
  // concurrency issues, the details of which are explained in the function.
  // If run_cuckoo returns ok (success), then `b` will be active, otherwise it
  // will not.
  template <typename TABLE_MODE>
  cuckoo_status run_cuckoo(TwoBuckets &b, size_type &insert_bucket,
                           size_type &insert_slot) {
    // We must unlock the buckets here, so that cuckoopath_search and
    // cuckoopath_move can lock buckets as desired without deadlock.
    // cuckoopath_move has to move something out of one of the original
    // buckets as its last operation, and it will lock both buckets and
    // leave them locked after finishing. This way, we know that if
    // cuckoopath_move succeeds, then the buckets needed for insertion are
    // still locked. If cuckoopath_move fails, the buckets are unlocked and
    // we try again. This unlocking does present two problems. The first is
    // that another insert on the same key runs and, finding that the key
    // isn't in the table, inserts the key into the table. Then we insert
    // the key into the table, causing a duplication. To check for this, we
    // search the buckets for the key we are trying to insert before doing
    // so (this is done in cuckoo_insert, and requires that both buckets are
    // locked). Another problem is that an expansion runs and changes the
    // hashpower, meaning the buckets may not be valid anymore. In this
    // case, the cuckoopath functions will have thrown a hashpower_changed
    // exception, which we catch and handle here.
    size_type hp = hashpower();
    b.unlock();
    CuckooRecords cuckoo_path;
    bool done = false;
    try {
      while (!done) {
        const int depth =
            cuckoopath_search<TABLE_MODE>(hp, cuckoo_path, b.i1, b.i2);
        if (depth < 0) {
          break;
        }

        if (cuckoopath_move<TABLE_MODE>(hp, cuckoo_path, depth, b)) {
          insert_bucket = cuckoo_path[0].bucket;
          insert_slot = cuckoo_path[0].slot;
          assert(insert_bucket == b.i1 || insert_bucket == b.i2);
          // assert(TABLE_MODE() == locked_table_mode() ||
          //        !get_current_locks()[lock_ind(b.i1)].try_lock());
          // assert(TABLE_MODE() == locked_table_mode() ||
          //        !get_current_locks()[lock_ind(b.i2)].try_lock());
          assert(!buckets_[insert_bucket].occupied(insert_slot));
          done = true;
          break;
        }
      }
    } catch (hashpower_changed &) {
      // The hashpower changed while we were trying to cuckoo, which means
      // we want to retry. b.i1 and b.i2 should not be locked
      // in this case.
      return failure_under_expansion;
    }
    return done ? ok : failure;
  }

  // cuckoopath_search finds a cuckoo path from one of the starting buckets to
  // an empty slot in another bucket. It returns the depth of the discovered
  // cuckoo path on success, and -1 on failure. Since it doesn't take locks on
  // the buckets it searches, the data can change between this function and
  // cuckoopath_move. Thus cuckoopath_move checks that the data matches the
  // cuckoo path before changing it.
  //
  // throws hashpower_changed if it changed during the search.
  template <typename TABLE_MODE>
  int cuckoopath_search(const size_type hp, CuckooRecords &cuckoo_path,
                        const size_type i1, const size_type i2) {
    b_slot x = slot_search<TABLE_MODE>(hp, i1, i2);
    if (x.depth == -1) {
      return -1;
    }
    // Fill in the cuckoo path slots from the end to the beginning.
    for (int i = x.depth; i >= 0; i--) {
      cuckoo_path[i].slot = x.pathcode % slot_per_bucket();
      x.pathcode /= slot_per_bucket();
    }
    // Fill in the cuckoo_path buckets and keys from the beginning to the
    // end, using the final pathcode to figure out which bucket the path
    // starts on. Since data could have been modified between slot_search
    // and the computation of the cuckoo path, this could be an invalid
    // cuckoo_path.
    CuckooRecord &first = cuckoo_path[0];
    if (x.pathcode == 0) {
      first.bucket = i1;
    } else {
      assert(x.pathcode == 1);
      first.bucket = i2;
    }
    {
      const auto lock_manager = lock_one(hp, first.bucket, TABLE_MODE());
      const bucket &b = buckets_[first.bucket];
      if (!b.occupied(first.slot)) {
        // We can terminate here
        return 0;
      }
      first.hv = hashed_key(b.key(first.slot));
    }
    for (int i = 1; i <= x.depth; ++i) {
      CuckooRecord &curr = cuckoo_path[i];
      const CuckooRecord &prev = cuckoo_path[i - 1];
      assert(prev.bucket == index_hash(hp, prev.hv.hash) ||
             prev.bucket ==
                 alt_index(hp, prev.hv.partial, index_hash(hp, prev.hv.hash)));
      // We get the bucket that this slot is on by computing the alternate
      // index of the previous bucket
      curr.bucket = alt_index(hp, prev.hv.partial, prev.bucket);
      const auto lock_manager = lock_one(hp, curr.bucket, TABLE_MODE());
      const bucket &b = buckets_[curr.bucket];
      if (!b.occupied(curr.slot)) {
        // We can terminate here
        return i;
      }
      curr.hv = hashed_key(b.key(curr.slot));
    }
    return x.depth;
  }

  // cuckoopath_move moves keys along the given cuckoo path in order to make
  // an empty slot in one of the buckets in cuckoo_insert. Before the start of
  // this function, the two insert-locked buckets were unlocked in run_cuckoo.
  // At the end of the function, if the function returns true (success), then
  // both insert-locked buckets remain locked. If the function is
  // unsuccessful, then both insert-locked buckets will be unlocked.
  //
  // throws hashpower_changed if it changed during the move.
  template <typename TABLE_MODE>
  bool cuckoopath_move(const size_type hp, CuckooRecords &cuckoo_path,
                       size_type depth, TwoBuckets &b) {
    if (depth == 0) {
      // There is a chance that depth == 0, when try_add_to_bucket sees
      // both buckets as full and cuckoopath_search finds one empty. In
      // this case, we lock both buckets. If the slot that
      // cuckoopath_search found empty isn't empty anymore, we unlock them
      // and return false. Otherwise, the bucket is empty and insertable,
      // so we hold the locks and return true.
      const size_type bucket_i = cuckoo_path[0].bucket;
      assert(bucket_i == b.i1 || bucket_i == b.i2);
      b = lock_two(hp, b.i1, b.i2, TABLE_MODE());
      if (!buckets_[bucket_i].occupied(cuckoo_path[0].slot)) {
        return true;
      } else {
        b.unlock();
        return false;
      }
    }

    while (depth > 0) {
      CuckooRecord &from = cuckoo_path[depth - 1];
      CuckooRecord &to = cuckoo_path[depth];
      const size_type fs = from.slot;
      const size_type ts = to.slot;
      TwoBuckets twob;
      LockManager extra_manager;
      if (depth == 1) {
        // Even though we are only swapping out of one of the original
        // buckets, we have to lock both of them along with the slot we
        // are swapping to, since at the end of this function, they both
        // must be locked. We store tb inside the extrab container so it
        // is unlocked at the end of the loop.
        std::tie(twob, extra_manager) =
            lock_three(hp, {{b.i1, b.i2, to.bucket}}, TABLE_MODE());
      } else {
        twob = lock_two(hp, from.bucket, to.bucket, TABLE_MODE());
      }

      bucket &fb = buckets_[from.bucket];
      bucket &tb = buckets_[to.bucket];

      // We plan to kick out fs, but let's check if it is still there;
      // there's a small chance we've gotten scooped by a later cuckoo. If
      // that happened, just... try again. Also the slot we are filling in
      // may have already been filled in by another thread, or the slot we
      // are moving from may be empty, both of which invalidate the swap.
      // We only need to check that the hash value is the same, because,
      // even if the keys are different and have the same hash value, then
      // the cuckoopath is still valid.
      if (tb.occupied(ts) || !fb.occupied(fs) ||
          hashed_key_only_hash(fb.key(fs)) != from.hv.hash) {
        return false;
      }

      buckets_.setKV(to.bucket, ts, fb.partial(fs), fb.movable_key(fs),
                     std::move(fb.mapped(fs)));
      buckets_.eraseKV(from.bucket, fs);
      if (depth == 1) {
        // Hold onto the locks contained in twob
        b = std::move(twob);
      }
      depth--;
    }
    return true;
  }

  // A constexpr version of pow that we can use for various compile-time
  // constants and checks.
  static constexpr size_type const_pow(size_type a, size_type b) {
    return (b == 0) ? 1 : a * const_pow(a, b - 1);
  }

  // b_slot holds the information for a BFS path through the table.
  struct b_slot {
    // The bucket of the last item in the path.
    size_type bucket;
    // a compressed representation of the slots for each of the buckets in
    // the path. pathcode is sort of like a base-slot_per_bucket number, and
    // we need to hold at most MAX_BFS_PATH_LEN slots. Thus we need the
    // maximum pathcode to be at least slot_per_bucket()^(MAX_BFS_PATH_LEN).
    uint16_t pathcode;
    static_assert(const_pow(slot_per_bucket(), MAX_BFS_PATH_LEN) <
                      std::numeric_limits<decltype(pathcode)>::max(),
                  "pathcode may not be large enough to encode a cuckoo "
                  "path");
    // The 0-indexed position in the cuckoo path this slot occupies. It must
    // be less than MAX_BFS_PATH_LEN, and also able to hold negative values.
    int8_t depth;
    static_assert(MAX_BFS_PATH_LEN - 1 <=
                      std::numeric_limits<decltype(depth)>::max(),
                  "The depth type must able to hold a value of"
                  " MAX_BFS_PATH_LEN - 1");
    static_assert(-1 >= std::numeric_limits<decltype(depth)>::min(),
                  "The depth type must be able to hold a value of -1");
    b_slot() {}
    b_slot(const size_type b, const uint16_t p, const decltype(depth) d)
        : bucket(b), pathcode(p), depth(d) {
      assert(d < MAX_BFS_PATH_LEN);
    }
  };

  // b_queue is the queue used to store b_slots for BFS cuckoo hashing.
  class b_queue {
  public:
    b_queue() noexcept : first_(0), last_(0) {}

    void enqueue(b_slot x) {
      assert(!full());
      slots_[last_++] = x;
    }

    b_slot dequeue() {
      assert(!empty());
      assert(first_ < last_);
      b_slot &x = slots_[first_++];
      return x;
    }

    bool empty() const { return first_ == last_; }

    bool full() const { return last_ == MAX_CUCKOO_COUNT; }

  private:
    // The size of the BFS queue. It holds just enough elements to fulfill a
    // MAX_BFS_PATH_LEN search for two starting buckets, with no circular
    // wrapping-around. For one bucket, this is the geometric sum
    // sum_{k=0}^{MAX_BFS_PATH_LEN-1} slot_per_bucket()^k
    // = (1 - slot_per_bucket()^MAX_BFS_PATH_LEN) / (1 - slot_per_bucket())
    //
    // Note that if slot_per_bucket() == 1, then this simply equals
    // MAX_BFS_PATH_LEN.
    static_assert(slot_per_bucket() > 0,
                  "SLOT_PER_BUCKET must be greater than 0.");
    static constexpr size_type MAX_CUCKOO_COUNT =
        2 * ((slot_per_bucket() == 1)
             ? MAX_BFS_PATH_LEN
             : (const_pow(slot_per_bucket(), MAX_BFS_PATH_LEN) - 1) /
               (slot_per_bucket() - 1));
    // An array of b_slots. Since we allocate just enough space to complete a
    // full search, we should never exceed the end of the array.
    b_slot slots_[MAX_CUCKOO_COUNT];
    // The index of the head of the queue in the array
    size_type first_;
    // One past the index of the last_ item of the queue in the array.
    size_type last_;
  };

  // slot_search searches for a cuckoo path using breadth-first search. It
  // starts with the i1 and i2 buckets, and, until it finds a bucket with an
  // empty slot, adds each slot of the bucket in the b_slot. If the queue runs
  // out of space, it fails.
  //
  // throws hashpower_changed if it changed during the search
  template <typename TABLE_MODE>
  b_slot slot_search(const size_type hp, const size_type i1,
                     const size_type i2) {
    b_queue q;
    // The initial pathcode informs cuckoopath_search which bucket the path
    // starts on
    q.enqueue(b_slot(i1, 0, 0));
    q.enqueue(b_slot(i2, 1, 0));
    while (!q.empty()) {
      b_slot x = q.dequeue();
      auto lock_manager = lock_one(hp, x.bucket, TABLE_MODE());
      bucket &b = buckets_[x.bucket];
      // Picks a (sort-of) random slot to start from
      size_type starting_slot = x.pathcode % slot_per_bucket();
      for (size_type i = 0; i < slot_per_bucket(); ++i) {
        uint16_t slot = (starting_slot + i) % slot_per_bucket();
        if (!b.occupied(slot)) {
          // We can terminate the search here
          x.pathcode = x.pathcode * slot_per_bucket() + slot;
          return x;
        }

        // If x has less than the maximum number of path components,
        // create a new b_slot item, that represents the bucket we would
        // have come from if we kicked out the item at this slot.
        const partial_t partial = b.partial(slot);
        if (x.depth < MAX_BFS_PATH_LEN - 1) {
          assert(!q.full());
          b_slot y(alt_index(hp, partial, x.bucket),
                   x.pathcode * slot_per_bucket() + slot, x.depth + 1);
          q.enqueue(y);
        }
      }
    }
    // We didn't find a short-enough cuckoo path, so the search terminated.
    // Return a failure value.
    return b_slot(0, 0, -1);
  }

  // cuckoo_fast_double will double the size of the table by taking advantage
  // of the properties of index_hash and alt_index. If the key's move
  // constructor is not noexcept, we use cuckoo_fast_double_throwable, since that
  // provides a strong exception guarantee.
  template <typename TABLE_MODE, typename AUTO_RESIZE>
  cuckoo_status cuckoo_fast_double(size_type current_hp) {
    const size_type new_hp = current_hp + 1;
    auto all_locks_manager = lock_all(TABLE_MODE());

    cuckoo_status st = check_resize_validity<AUTO_RESIZE>(current_hp, new_hp);
    if (st != ok) {
      return st;
    }

    if constexpr (!is_data_nothrow_move_constructible()) {
      LIBCUCKOO_DBG("%s", "cannot run cuckoo_fast_double because key-value"
                          " pair is not nothrow move constructible");
      cuckoo_fast_double_throwable<TABLE_MODE, AUTO_RESIZE>(current_hp);
      return ok;
    }

    // Finish rehashing any un-rehashed buckets, so that we can move out any
    // remaining data in old_buckets_.  We should be running cuckoo_fast_double
    // only after trying to cuckoo for a while, which should mean we've tried
    // going through most of the table and thus done a lot of rehashing
    // already. So this shouldn't be too expensive.
    //
    // We restrict ourselves to the current thread because we want to avoid
    // possibly spawning extra threads in this function, unless the
    // circumstances are predictable (i.e. data is nothrow move constructible,
    // we're in locked_table mode and must keep the buckets_ container
    // up-to-date, etc).
    //
    // If we have fewer than kNumLocks buckets, there shouldn't be any buckets
    // left to rehash, so this should be a no-op.
    rehash_all(current_hp);


    // Resize the locks array if necessary. This is done before we update the
    // hashpower so that other threads don't grab the new hashpower and the old
    // locks.
    maybe_resize_locks();

    
    buckets_.double_size();

    // If we have less than kMaxNumLocks buckets, we do a full rehash in the
    // current thread. On-demand rehashing wouldn't be very easy with less than
    // kMaxNumLocks buckets, because it would require taking extra lower-index
    // locks to do the rehashing. Because kMaxNumLocks is relatively small,
    // this should not be very expensive. We have already set all locks to
    // migrated at the start of the function, so we shouldn't have to touch
    // them again.
    //
    // Otherwise, if we're in locked_table_mode, the expectation is that we can
    // access the latest data in buckets_ without taking any locks. So we must
    // rehash the data immediately. This would not be much different from
    // lazy-rehashing in locked_table_mode anyways, because it would still be
    // going on in one thread.
    if (current_hp < kMaxNumLocksPow) {
      auto it_old = buckets_.begin();
      auto it_new = buckets_.get_iterator(hashsize(current_hp));

      auto it_lock_old = locks_.begin();
      auto it_lock_new = locks_.get_iterator(hashsize(current_hp));
      for (size_type old_bucket_ind = 0; old_bucket_ind < hashsize(current_hp); 
           ++old_bucket_ind, ++it_old, ++it_new, ++it_lock_old, ++it_lock_new) {
        move_bucket(current_hp, old_bucket_ind, *it_old, *it_new,
          [this, &it_old, &it_lock_old, &it_lock_new](size_type old_bucket_slot) {
            buckets_.eraseKV(*it_old, old_bucket_slot);
            --(it_lock_old->elem_counter());
            ++(it_lock_new->elem_counter());
          });
      }
    } else {
      // Mark all current locks as un-migrated, so that we rehash the data
      // on-demand when the locks are taken.
      for (seqlock &lock : locks_) {
        lock.set_migrated(false);
      }
      
      if constexpr (std::is_same<TABLE_MODE, locked_table_mode>::value) {
        rehash_with_workers();
      }
    }
    
    return ok;
  }

  // Does not take any locks, because calls only from cuckoo_fast_double
  template <typename TABLE_MODE, typename AUTO_RESIZE>
  void cuckoo_fast_double_throwable(size_type current_hp) {
    auto cur_pointer = buckets_.allocate_and_construct(current_hp + 1);
    const size_type num_workers = 1 + max_num_worker_threads();

    if (current_hp < kMaxNumLocksPow) {
      std::vector<std::pair<size_type, size_type>> to_erase;

      auto it_old = buckets_.begin();
      try {
        for (size_type old_bucket_ind = 0; old_bucket_ind < hashsize(current_hp); 
            ++old_bucket_ind, ++it_old) {
          move_bucket(current_hp, old_bucket_ind, *it_old, cur_pointer[old_bucket_ind],
            [old_bucket_ind, &to_erase](size_type old_bucket_slot) {
              to_erase.emplace_back(old_bucket_ind, old_bucket_slot);
            }
          );
        }
      } catch (...) {
        buckets_.destroy_and_deallocate(cur_pointer, current_hp + 1);
        throw;
      }

      maybe_resize_locks();

      buckets_.push_back(cur_pointer);

      for (const auto& [old_bucket_ind, old_bucket_slot] : to_erase) {
        buckets_.eraseKV(old_bucket_ind, old_bucket_slot);
        --(locks_[old_bucket_ind]).elem_counter();
        ++(locks_[old_bucket_ind + hashsize(current_hp)]).elem_counter();
      }
    } else {
      std::vector<std::vector<std::pair<size_type, size_type>>> to_erase_segmented(num_workers);
      std::atomic_uint32_t num(0);

      try {
        parallel_exec(
          0, hashsize(current_hp),
          [this, current_hp, &cur_pointer, &to_erase_segmented, &num](size_type old_bucket_ind, size_type end, std::exception_ptr &eptr) {
            auto& to_erase = to_erase_segmented[num.fetch_add(1, std::memory_order_acq_rel)];
            try {
              for (; old_bucket_ind < end; ++old_bucket_ind) {
                move_bucket(
                  current_hp,
                  old_bucket_ind, 
                  buckets_[old_bucket_ind], 
                  cur_pointer[old_bucket_ind], 
                  [current_hp, old_bucket_ind, &to_erase](size_type old_bucket_slot) {
                    to_erase.emplace_back(old_bucket_ind, old_bucket_slot);
                  }
                );
              }
            } catch (...) {
              eptr = std::current_exception();
            }
          }
        );
      } catch (...) {
        buckets_.destroy_and_deallocate(cur_pointer, current_hp + 1);
        throw;
      }
      buckets_.push_back(cur_pointer);

      parallel_exec_noexcept(
        0, num_workers, 
        [this, current_hp, &to_erase_segmented](size_type ind, size_type) {
          for (const auto& [old_bucket_ind, old_bucket_slot] : to_erase_segmented[ind]) {
            buckets_.eraseKV(old_bucket_ind, old_bucket_slot);
          }
        }
      );
    }
  }

  static inline bool need_to_move_elem(const hash_value& hv, 
      size_type old_hp, size_type new_hp,
      size_type old_bucket_ind, size_type new_bucket_ind) noexcept{
    const size_type old_ihash = index_hash(old_hp, hv.hash);
    const size_type old_ahash = alt_index(old_hp, hv.partial, old_ihash);
    const size_type new_ihash = index_hash(new_hp, hv.hash);
    const size_type new_ahash = alt_index(new_hp, hv.partial, new_ihash);

    return (old_bucket_ind == old_ihash && new_ihash == new_bucket_ind) ||
           (old_bucket_ind == old_ahash && new_ahash == new_bucket_ind);
  }

  template <typename F>
  void move_bucket(size_type old_hp, size_type old_bucket_ind, 
      bucket& old_bucket, bucket& new_bucket, F after_set = (size_type){}) const {
    const size_t new_hp = old_hp + 1;

    // By doubling the table size, the index_hash and alt_index of each key got
    // one bit added to the top, at position old_hp, which means anything we
    // have to move will either be at the same bucket position, or exactly
    // hashsize(old_hp) later than the current bucket.
    const size_type new_bucket_ind = old_bucket_ind + hashsize(old_hp);
    size_type new_bucket_slot = 0;

    // For each occupied slot, either move it into its same position in the
    // new buckets container, or to the first available spot in the new
    // bucket in the new buckets container.
    for (size_type old_bucket_slot = 0; old_bucket_slot < slot_per_bucket(); ++old_bucket_slot) {
      if (old_bucket.occupied(old_bucket_slot) && 
          need_to_move_elem(hashed_key(old_bucket.key(old_bucket_slot)), 
                            old_hp, new_hp, 
                            old_bucket_ind, new_bucket_ind)) {
        // We're moving the key to the new bucket
        buckets_.setKV(new_bucket,
                       new_bucket_slot++, 
                       old_bucket.partial(old_bucket_slot), 
                       old_bucket.movable_key(old_bucket_slot), 
                       std::move(old_bucket.mapped(old_bucket_slot)));
        
        after_set(old_bucket_slot);
      }
    }
  }

  // Checks whether the resize is okay to proceed. Returns a status code, or
  // throws an exception, depending on the error type.
  using automatic_resize = std::integral_constant<bool, true>;
  using manual_resize = std::integral_constant<bool, false>;

  template <typename AUTO_RESIZE>
  cuckoo_status check_resize_validity(const size_type orig_hp,
                                      const size_type new_hp) {
    const size_type mhp = maximum_hashpower();
    if (mhp != NO_MAXIMUM_HASHPOWER && new_hp > mhp) {
      throw maximum_hashpower_exceeded(new_hp);
    }
    if (AUTO_RESIZE::value && load_factor() < minimum_load_factor()) {
      throw load_factor_too_low(minimum_load_factor());
    }
    if (hashpower() != orig_hp) {
      // Most likely another expansion ran before this one could grab the
      // locks
      LIBCUCKOO_DBG("%s", "another expansion is on-going\n");
      return failure_under_expansion;
    }
    return ok;
  }

  // When we expand the contanier, we may need to expand the locks array, if
  // the current locks array is smaller than the maximum size and also smaller
  // than the number of buckets in the upcoming buckets container. In this
  // case, we grow the locks array to the smaller of the maximum lock array
  // size and the bucket count.
  void maybe_resize_locks() {
    if (hashpower() >= kMaxNumLocksPow) {
      return;
    }
  
    locks_.double_size(true, true);
  }

  template <typename TABLE_MODE, typename AUTO_RESIZE>
  cuckoo_status cuckoo_expand_simple(size_type new_hp) {
    assert(hashpower() < new_hp);

    auto all_locks_manager = lock_all(TABLE_MODE());
    for (int32_t current_hp = hashpower(); current_hp < new_hp; ++current_hp) {
      auto st = cuckoo_fast_double<locked_table_mode, AUTO_RESIZE>(current_hp);
      if (st != ok) {
        return st;
      }
    }

    return ok;
  }

  // cuckoo_change_capacity will resize the table to at least the given
  // new_hashpower. When we're shrinking the table, if the current table
  // contains more elements than can be held by new_hashpower, the resulting
  // hashpower will be greater than `new_hp`. It needs to take all the bucket
  // locks, since no other operations can change the table during expansion.
  // Throws maximum_hashpower_exceeded if we're expanding beyond the
  // maximum hashpower, and we have an actual limit.
  template <typename TABLE_MODE, typename AUTO_RESIZE>
  cuckoo_status cuckoo_change_capacity(size_type new_hp) {
    auto all_locks_manager = lock_all(TABLE_MODE());
    const size_type hp = hashpower();
    cuckoo_status st = check_resize_validity<AUTO_RESIZE>(hp, new_hp);
    if (st != ok) {
      return st;
    }

    // Finish rehashing any data into buckets_.
    rehash_all(hp);

    // Creates a new hash table with hashpower new_hp and adds all the elements
    // from buckets_ and old_buckets_. Allow this map to spawn extra threads if
    // it needs to resize during the resize.
    cuckoohash_map new_map(hashsize(new_hp) * slot_per_bucket(),
                           hash_function(), key_eq(), get_allocator());
    new_map.max_num_worker_threads(max_num_worker_threads());

    parallel_exec(
        0, hashsize(hp),
        [this, &new_map]
        (size_type i, size_type end, std::exception_ptr &eptr) {
          try {
            for (; i < end; ++i) {
              auto &bucket = buckets_[i];
              for (size_type j = 0; j < slot_per_bucket(); ++j) {
                if (bucket.occupied(j)) {
                  new_map.insert(bucket.movable_key(j),
                                 std::move(bucket.mapped(j)));
                }
              }
            }
          } catch (...) {
            eptr = std::current_exception();
          }
        });

    auto new_map_all_locks_manager = new_map.lock_all(normal_mode());

    locks_.swap(new_map.locks_);
    buckets_.swap(new_map.buckets_);
    return ok;
  }

  // Executes the function over the given range, splitting the work between the
  // current thread and any available worker threads.
  //
  // In the noexcept version, the functor must implement operator()(size_type
  // start, size_type end).
  //
  // In the non-noexcept version, the functor will receive an additional
  // std::exception_ptr& argument.

  template <typename F>
  void parallel_exec_noexcept(size_type start, size_type end, F func) {
    const size_type num_extra_threads = max_num_worker_threads();
    const size_type num_workers = 1 + num_extra_threads;
    size_type work_per_thread = (end - start) / num_workers;
    std::vector<std::thread, rebind_alloc<std::thread>> threads(
        get_allocator());
    threads.reserve(num_extra_threads);
    for (size_type i = 0; i < num_extra_threads; ++i) {
      threads.emplace_back(func, start, start + work_per_thread);
      start += work_per_thread;
    }
    func(start, end);
    for (std::thread &t : threads) {
      t.join();
    }
  }

  template <typename F>
  void parallel_exec(size_type start, size_type end, F func) {
    const size_type num_extra_threads = max_num_worker_threads();
    const size_type num_workers = 1 + num_extra_threads;
    size_type work_per_thread = (end - start) / num_workers;
    std::vector<std::thread, rebind_alloc<std::thread>> threads(
        get_allocator());
    threads.reserve(num_extra_threads);

    std::vector<std::exception_ptr, rebind_alloc<std::exception_ptr>> eptrs(
        num_workers, nullptr, get_allocator());
    for (size_type i = 0; i < num_extra_threads; ++i) {
      threads.emplace_back(func, start, start + work_per_thread,
                           std::ref(eptrs[i]));
      start += work_per_thread;
    }
    func(start, end, std::ref(eptrs.back()));
    for (std::thread &t : threads) {
      t.join();
    }
    for (std::exception_ptr &eptr : eptrs) {
      if (eptr) std::rethrow_exception(eptr);
    }
  }

  // Does a batch resize of the remaining data in old_buckets_. Assumes all the
  // locks have already been taken.
  void rehash_with_workers() noexcept {
    parallel_exec_noexcept(
        0, locks_.size(),
        [this](size_type start, size_type end) {
          for (size_type i = start; i < end; ++i) {
            lock_and_rehash<locked_table_mode>(i);
          }
        });
  }

  void rehash_all(size_type current_hp) noexcept {
    if (current_hp > kMaxNumLocksPow) {
      rehash_with_workers();
    }
  }

  // Deletion functions

  // Removes an item from a bucket, decrementing the associated counter as
  // well.
  void del_from_bucket(seqlock& lock_, bucket& bucket_, const size_type slot) const {
    buckets_.eraseKV(bucket_, slot);
    --lock_.elem_counter();
  }

  void del_from_bucket(const size_type index, const size_type slot) const {
    buckets_.eraseKV(index, slot);
    --locks_[lock_ind(index)].elem_counter();
  }

  // Empties the table, calling the destructors of all the elements it removes
  // from the table. It assumes the locks are taken as necessary.
  void cuckoo_clear() {
    buckets_.clear();
    for (seqlock &lock : locks_) {
      lock.elem_counter() = 0;
      lock.set_migrated(true);
    }
  }

  // Rehashing functions

  template <typename TABLE_MODE> bool cuckoo_rehash_concurrent(size_type n) {
    const size_type hp = hashpower();
    if (n <= hp) {
      return false;
    }
    return cuckoo_expand_simple<TABLE_MODE, manual_resize>(n) == ok;
  }
  template <typename TABLE_MODE> bool cuckoo_rehash(size_type n) {
    const size_type hp = hashpower();
    if (n == hp) {
      return false;
    }
    return cuckoo_change_capacity<TABLE_MODE, manual_resize>(n) == ok;
  }

  template <typename TABLE_MODE> bool cuckoo_reserve_concurrent(size_type n) {
    const size_type hp = hashpower();
    const size_type new_hp = reserve_calc(n);
    if (new_hp <= hp) {
      return false;
    }
    return cuckoo_expand_simple<TABLE_MODE, manual_resize>(new_hp) == ok;
  }
  template <typename TABLE_MODE> bool cuckoo_reserve(size_type n) {
    const size_type hp = hashpower();
    const size_type new_hp = reserve_calc(n);
    if (new_hp == hp) {
      return false;
    }
    return cuckoo_change_capacity<TABLE_MODE, manual_resize>(new_hp) == ok;
  }

  static size_type reserve_calc(const size_type n) {
    return reserve_calc_for_slots<SLOT_PER_BUCKET>(n);
  }

  // This class is a friend for unit testing
  friend class UnitTestInternalAccess;
  
  friend class locked_table;

  static constexpr size_type kMaxNumLocksPow = 16;
  static constexpr size_type kMaxNumLocks = 1UL << kMaxNumLocksPow;

  // Member variables

  // The hash function
  hasher hash_fn_;

  // The equality function
  key_equal eq_fn_;

  // container of buckets. The size or memory location of the buckets cannot be
  // changed unless all the locks are taken on the table. Thus, it is only safe
  // to access the buckets_ container when you have at least one lock held.
  //
  // Marked mutable so that const methods can rehash into this container when
  // necessary.
  mutable buckets_t buckets_;

  // A linked list of all lock containers. We never discard lock containers,
  // since there is currently no mechanism for detecting when all threads are
  // done looking at the memory. The back lock container in this list is
  // designated the "current" one, and is used by all operations taking locks.
  // This container can be modified if either it is empty (which should only
  // occur during construction), or if the modifying thread has taken all the
  // locks on the existing "current" container. In the latter case, a
  // modification must take place before a modification to the hashpower, so
  // that other threads can detect the change and adjust appropriately. Marked
  // mutable so that const methods can access and take locks.
  mutable locks_t locks_;

  // Stores the minimum load factor allowed for automatic expansions. Whenever
  // an automatic expansion is triggered (during an insertion where cuckoo
  // hashing fails, for example), we check the load factor against this
  // double, and throw an exception if it's lower than this value. It can be
  // used to signal when the hash function is bad or the input adversarial.
  CopyableAtomic<double> minimum_load_factor_;

  // stores the maximum hashpower allowed for any expansions. If set to
  // NO_MAXIMUM_HASHPOWER, this limit will be disregarded.
  CopyableAtomic<size_type> maximum_hashpower_;

  // Maximum number of extra threads to spawn when doing any large batch
  // operations.
  CopyableAtomic<size_type> max_num_worker_threads_;

public:
  /**
   * An ownership wrapper around a @ref cuckoohash_map table instance. When
   * given a table instance, it takes all the locks on the table, blocking all
   * outside operations on the table. Because the locked_table has unique
   * ownership of the table, it can provide a set of operations on the table
   * that aren't possible in a concurrent context.
   *
   * The locked_table interface is very similar to the STL unordered_map
   * interface, and for functions whose signatures correspond to unordered_map
   * methods, the behavior should be mostly the same.
   */
  class locked_table {
  public:
    /** @name Type Declarations */
    /**@{*/

    using key_type = typename cuckoohash_map::key_type;
    using mapped_type = typename cuckoohash_map::mapped_type;
    using value_type = typename cuckoohash_map::value_type;
    using size_type = typename cuckoohash_map::size_type;
    using difference_type = typename cuckoohash_map::difference_type;
    using hasher = typename cuckoohash_map::hasher;
    using key_equal = typename cuckoohash_map::key_equal;
    using allocator_type = typename cuckoohash_map::allocator_type;
    using reference = typename cuckoohash_map::reference;
    using const_reference = typename cuckoohash_map::const_reference;
    using pointer = typename cuckoohash_map::pointer;
    using const_pointer = typename cuckoohash_map::const_pointer;

    /**
     * A constant iterator over a @ref locked_table, which allows read-only
     * access to the elements of the table. It fulfills the
     * BidirectionalIterator concept.
     */
    class const_iterator {
    public:
      using difference_type = typename locked_table::difference_type;
      using value_type = typename locked_table::value_type;
      using pointer = typename locked_table::const_pointer;
      using reference = typename locked_table::const_reference;
      using iterator_category = std::bidirectional_iterator_tag;

      const_iterator() {}

      // Return true if the iterators are from the same locked table and
      // location, false otherwise.
      bool operator==(const const_iterator &it) const {
        return buckets_ == it.buckets_ && index_ == it.index_ &&
               slot_ == it.slot_;
      }

      bool operator!=(const const_iterator &it) const {
        return !(operator==(it));
      }

      reference operator*() const { return (*buckets_)[index_].kvpair(slot_); }

      pointer operator->() const { return std::addressof(operator*()); }

      // Advance the iterator to the next item in the table, or to the end
      // of the table. Returns the iterator at its new position.
      const_iterator &operator++() {
        // Move forward until we get to a slot that is occupied, or we
        // get to the end
        ++slot_;
        for (; index_ < buckets_->size(); ++index_) {
          for (; slot_ < slot_per_bucket(); ++slot_) {
            if ((*buckets_)[index_].occupied(slot_)) {
              return *this;
            }
          }
          slot_ = 0;
        }
        assert(std::make_pair(index_, slot_) == end_pos(*buckets_));
        return *this;
      }

      // Advance the iterator to the next item in the table, or to the end
      // of the table. Returns the iterator at its old position.
      const_iterator operator++(int) {
        const_iterator old(*this);
        ++(*this);
        return old;
      }

      // Move the iterator back to the previous item in the table. Returns
      // the iterator at its new position.
      const_iterator &operator--() {
        // Move backward until we get to the beginning. Behavior is
        // undefined if we are iterating at the first element, so we can
        // assume we'll reach an element. This means we'll never reach
        // index_ == 0 and slot_ == 0.
        if (slot_ == 0) {
          --index_;
          slot_ = slot_per_bucket() - 1;
        } else {
          --slot_;
        }
        while (!(*buckets_)[index_].occupied(slot_)) {
          if (slot_ == 0) {
            --index_;
            slot_ = slot_per_bucket() - 1;
          } else {
            --slot_;
          }
        }
        return *this;
      }

      //! Move the iterator back to the previous item in the table.
      //! Returns the iterator at its old position. Behavior is undefined
      //! if the iterator is at the beginning.
      const_iterator operator--(int) {
        const_iterator old(*this);
        --(*this);
        return old;
      }

    protected:
      // The buckets owned by the locked table being iterated over. Even
      // though const_iterator cannot modify the buckets, we don't mark
      // them const so that the mutable iterator can derive from this
      // class. Also, since iterators should be default constructible,
      // copyable, and movable, we have to make this a raw pointer type.
      buckets_t *buckets_;

      // The bucket index of the item being pointed to. For implementation
      // convenience, we let it take on negative values.
      size_type index_;

      // The slot in the bucket of the item being pointed to. For
      // implementation convenience, we let it take on negative values.
      size_type slot_;

      // Returns the position signifying the end of the table
      static std::pair<size_type, size_type> end_pos(const buckets_t &buckets) {
        return std::make_pair(buckets.size(), 0);
      }

      // The private constructor is used by locked_table to create
      // iterators from scratch. If the given index_-slot_ pair is at the
      // end of the table, or the given spot is occupied, stay. Otherwise,
      // step forward to the next data item, or to the end of the table.
      const_iterator(buckets_t &buckets, size_type index,
                     size_type slot) noexcept
          : buckets_(std::addressof(buckets)), index_(index), slot_(slot) {
        if (std::make_pair(index_, slot_) != end_pos(*buckets_) &&
            !(*buckets_)[index_].occupied(slot_)) {
          operator++();
        }
      }

      friend class locked_table;
    };

    /**
     * An iterator over a @ref locked_table, which allows read-write access
     * to elements of the table. It fulfills the BidirectionalIterator
     * concept.
     */
    class iterator : public const_iterator {
    public:
      using pointer = typename cuckoohash_map::pointer;
      using reference = typename cuckoohash_map::reference;

      iterator() {}

      bool operator==(const iterator &it) const {
        return const_iterator::operator==(it);
      }

      bool operator!=(const iterator &it) const {
        return const_iterator::operator!=(it);
      }

      reference operator*() {
        return (*const_iterator::buckets_)[const_iterator::index_].kvpair(
            const_iterator::slot_);
      }

      pointer operator->() { return std::addressof(operator*()); }

      iterator &operator++() {
        const_iterator::operator++();
        return *this;
      }

      iterator operator++(int) {
        iterator old(*this);
        const_iterator::operator++();
        return old;
      }

      iterator &operator--() {
        const_iterator::operator--();
        return *this;
      }

      iterator operator--(int) {
        iterator old(*this);
        const_iterator::operator--();
        return old;
      }

    private:
      iterator(buckets_t &buckets, size_type index, size_type slot) noexcept
          : const_iterator(buckets, index, slot) {}

      friend class locked_table;
    };

    /**@}*/

    /** @name Table Parameters */
    /**@{*/

    static constexpr size_type slot_per_bucket() {
      return cuckoohash_map::slot_per_bucket();
    }

    /**@}*/

    /** @name Constructors, Destructors, and Assignment */
    /**@{*/

    locked_table() = delete;
    locked_table(const locked_table &) = delete;
    locked_table &operator=(const locked_table &) = delete;

    locked_table(locked_table &&lt) noexcept
        : map_(std::move(lt.map_)),
          all_locks_manager_(std::move(lt.all_locks_manager_)) {}

    locked_table &operator=(locked_table &&lt) noexcept {
      unlock();
      map_ = std::move(lt.map_);
      all_locks_manager_ = std::move(lt.all_locks_manager_);
      return *this;
    }

    /**
     * Unlocks the table, thereby freeing the locks on the table, but also
     * invalidating all iterators and table operations with this object. It
     * is idempotent.
     */
    void unlock() { all_locks_manager_.reset(); }

    /**@}*/

    /** @name Table Details
     *
     * Methods for getting information about the table. Many are identical
     * to their @ref cuckoohash_map counterparts. Only new functions or
     * those with different behavior are documented.
     *
     */
    /**@{*/

    /**
     * Returns whether the locked table has ownership of the table
     *
     * @return true if it still has ownership, false otherwise
     */
    bool is_active() const { return static_cast<bool>(all_locks_manager_); }

    hasher hash_function() const { return map_.get().hash_function(); }

    key_equal key_eq() const { return map_.get().key_eq(); }

    allocator_type get_allocator() const { return map_.get().get_allocator(); }

    size_type hashpower() const { return map_.get().hashpower(); }

    size_type bucket_count() const { return map_.get().bucket_count(); }

    bool empty() const { return map_.get().empty(); }

    size_type size() const { return map_.get().size(); }

    size_type capacity() const { return map_.get().capacity(); }

    double load_factor() const { return map_.get().load_factor(); }

    void minimum_load_factor(const double mlf) {
      map_.get().minimum_load_factor(mlf);
    }

    double minimum_load_factor() const {
      return map_.get().minimum_load_factor();
    }

    void maximum_hashpower(size_type mhp) { map_.get().maximum_hashpower(mhp); }

    size_type maximum_hashpower() const {
      return map_.get().maximum_hashpower();
    }

    void max_num_worker_threads(size_type extra_threads) {
      map_.get().max_num_worker_threads(extra_threads);
    }

    size_type max_num_worker_threads() const {
      return map_.get().max_num_worker_threads();
    }

    /**@}*/

    /** @name Iterators */
    /**@{*/

    /**
     * Returns an iterator to the beginning of the table. If the table is
     * empty, it will point past the end of the table.
     *
     * @return an iterator to the beginning of the table
     */

    iterator begin() { return iterator(map_.get().buckets_, 0, 0); }

    const_iterator begin() const {
      return const_iterator(map_.get().buckets_, 0, 0);
    }

    const_iterator cbegin() const { return begin(); }

    /**
     * Returns an iterator past the end of the table.
     *
     * @return an iterator past the end of the table
     */

    iterator end() {
      const auto end_pos = const_iterator::end_pos(map_.get().buckets_);
      return iterator(map_.get().buckets_,
                      static_cast<size_type>(end_pos.first),
                      static_cast<size_type>(end_pos.second));
    }

    const_iterator end() const {
      const auto end_pos = const_iterator::end_pos(map_.get().buckets_);
      return const_iterator(map_.get().buckets_,
                            static_cast<size_type>(end_pos.first),
                            static_cast<size_type>(end_pos.second));
    }

    const_iterator cend() const { return end(); }

    /**@}*/

    /** @name Modifiers */
    /**@{*/

    void clear() { map_.get().cuckoo_clear(); }

    /**
     * This behaves like the @c unordered_map::try_emplace method.  It will
     * always invalidate all iterators, due to the possibilities of cuckoo
     * hashing and expansion.
     */
    template <typename K, typename... Args>
    std::pair<iterator, bool> insert(K &&key, Args &&... val) {
      hash_value hv = map_.get().hashed_key(key);
      TwoBuckets b = map_.get().template snapshot_and_lock_two<locked_table_mode>(hv);
      table_position pos =
          map_.get().template cuckoo_insert_loop<locked_table_mode>(hv, b, key);
      
      if (pos.status == ok) {
        map_.get().add_to_bucket(pos.index, 
                                 pos.slot, 
                                 hv.partial,
                                 std::forward<K>(key),
                                 std::forward<Args>(val)...);
      } else {
        assert(pos.status == failure_key_duplicated);
      }

      return std::make_pair(
        iterator(map_.get().buckets_, pos.index, pos.slot),
        pos.status == ok
      );
    }

    iterator erase(const_iterator pos) {
      map_.get().del_from_bucket(pos.index_, pos.slot_);
      return iterator(map_.get().buckets_, pos.index_, pos.slot_);
    }

    iterator erase(iterator pos) {
      map_.get().del_from_bucket(pos.index_, pos.slot_);
      return iterator(map_.get().buckets_, pos.index_, pos.slot_);
    }

    template <typename K> size_type erase(const K &key) {
      const hash_value hv = map_.get().hashed_key(key);
      const auto b =
          map_.get().template snapshot_and_lock_two<locked_table_mode>(hv);
      const table_position pos =
          map_.get().cuckoo_find(key, hv.partial, b.i1, b.i2);
      if (pos.status == ok) {
        map_.get().del_from_bucket(pos.index, pos.slot);
        return 1;
      } else {
        return 0;
      }
    }

    /**@}*/

    /** @name Lookup */
    /**@{*/

    template <typename K> iterator find(const K &key) {
      const hash_value hv = map_.get().hashed_key(key);
      
      const auto b =
          map_.get().template snapshot_and_lock_two<locked_table_mode>(hv);
      const table_position pos =
          map_.get().cuckoo_find(key, hv.partial, b.i1, b.i2);
      if (pos.status == ok) {
        return iterator(map_.get().buckets_, pos.index, pos.slot);
      } else {
        return end();
      }
    }

    template <typename K> const_iterator find(const K &key) const {
      const hash_value hv = map_.get().hashed_key(key);
      const auto b =
          map_.get().template snapshot_and_lock_two<locked_table_mode>(hv);
      const table_position pos =
          map_.get().cuckoo_find(key, hv.partial, b.i1, b.i2);
      if (pos.status == ok) {
        return const_iterator(map_.get().buckets_, pos.index, pos.slot);
      } else {
        return end();
      }
    }

    template <typename K> mapped_type &at(const K &key) {
      auto it = find(key);
      if (it == end()) {
        throw std::out_of_range("key not found in table");
      } else {
        return it->second;
      }
    }

    template <typename K> const mapped_type &at(const K &key) const {
      auto it = find(key);
      if (it == end()) {
        throw std::out_of_range("key not found in table");
      } else {
        return it->second;
      }
    }

    /**
     * This function has the same lifetime properties as @ref
     * cuckoohash_map::insert, except that the value is default-constructed,
     * with no parameters, if it is not already in the table.
     */
    template <typename K> T &operator[](K &&key) {
      auto result = insert(std::forward<K>(key));
      return result.first->second;
    }

    template <typename K> size_type count(const K &key) const {
      const hash_value hv = map_.get().hashed_key(key);
      const auto b =
          map_.get().template snapshot_and_lock_two<locked_table_mode>(hv);
      return map_.get().cuckoo_find(key, hv.partial, b.i1, b.i2).status == ok
                 ? 1
                 : 0;
    }

    template <typename K>
    std::pair<iterator, iterator> equal_range(const K &key) {
      auto it = find(key);
      if (it == end()) {
        return std::make_pair(it, it);
      } else {
        auto start_it = it++;
        return std::make_pair(start_it, it);
      }
    }

    template <typename K>
    std::pair<const_iterator, const_iterator> equal_range(const K &key) const {
      auto it = find(key);
      if (it == end()) {
        return std::make_pair(it, it);
      } else {
        auto start_it = it++;
        return std::make_pair(start_it, it);
      }
    }

    /**@}*/

    /** @name Re-sizing */
    /**@{*/

    /**
     * This has the same behavior as @ref cuckoohash_map::rehash, except
     * that we don't return anything.
     */
    void rehash(size_type n) {
      map_.get().template cuckoo_rehash<locked_table_mode>(n);
    }

    /**
     * This has the same behavior as @ref cuckoohash_map::reserve, except
     * that we don't return anything.
     */
    void reserve(size_type n) {
      map_.get().template cuckoo_reserve<locked_table_mode>(n);
    }

    /**@}*/

    /** @name Comparison  */
    /**@{*/

    bool operator==(const locked_table &lt) const {
      if (size() != lt.size()) {
        return false;
      }
      for (const auto &elem : lt) {
        auto it = find(elem.first);
        if (it == end() || it->second != elem.second) {
          return false;
        }
      }
      return true;
    }

    bool operator!=(const locked_table &lt) const {
      if (size() != lt.size()) {
        return true;
      }
      for (const auto &elem : lt) {
        auto it = find(elem.first);
        if (it == end() || it->second != elem.second) {
          return true;
        }
      }
      return false;
    }

    /**@}*/

  private:
    // The constructor locks the entire table. We keep this constructor private
    // (but expose it to the cuckoohash_map class), since we don't want users
    // calling it. We also complete any remaining rehashing in the table, so
    // that everything is in map.buckets_.
    locked_table(cuckoohash_map &map) noexcept
        : map_(map),
          all_locks_manager_(map.lock_all(normal_mode())) {
      map.rehash_with_workers();
    }

    // Dispatchers for methods on cuckoohash_map

    buckets_t &buckets() { return map_.get().buckets_; }

    const buckets_t &buckets() const { return map_.get().buckets_; }

    void maybe_resize_locks() {
      map_.get().maybe_resize_locks();
    }

    // A reference to the map owned by the table
    std::reference_wrapper<cuckoohash_map> map_;
    // A manager for all the locks we took on the table.
    AllLocksManager all_locks_manager_;

    friend class cuckoohash_map;
  };
};

/**
 * Specializes the @c std::swap algorithm for @c cuckoohash_map. Calls @c
 * lhs.swap(rhs).
 *
 * @param lhs the map on the left side to swap
 * @param lhs the map on the right side to swap
 */
template <class Key, class T, class Hash, class KeyEqual, class Allocator,
          std::size_t SLOT_PER_BUCKET>
void swap(
    cuckoohash_map<Key, T, Hash, KeyEqual, Allocator, SLOT_PER_BUCKET> &lhs,
    cuckoohash_map<Key, T, Hash, KeyEqual, Allocator, SLOT_PER_BUCKET>
        &rhs) noexcept {
  lhs.swap(rhs);
}

}  // namespace seqlock_lib::cuckoo
