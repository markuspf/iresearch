//
// IResearch search engine 
// 
// Copyright � 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#ifndef IRESEARCH_DIRECTORY_H
#define IRESEARCH_DIRECTORY_H

#include "data_input.hpp"
#include "data_output.hpp"
#include "utils/attributes_provider.hpp"
#include "utils/memory.hpp"
#include "utils/noncopyable.hpp"
#include "utils/string.hpp"

#include <vector>

NS_ROOT

//////////////////////////////////////////////////////////////////////////////
/// @struct index_lock 
/// @brief an interface for abstract resource locking
//////////////////////////////////////////////////////////////////////////////
struct IRESEARCH_API index_lock : private util::noncopyable {
  DECLARE_IO_PTR(index_lock, unlock);
  DECLARE_FACTORY(index_lock);

  static const size_t LOCK_WAIT_FOREVER = integer_traits<size_t>::const_max;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief destructor 
  ////////////////////////////////////////////////////////////////////////////
  virtual ~index_lock();

  ////////////////////////////////////////////////////////////////////////////
  /// @brief locks the guarded resource
  /// @returns true on success
  ////////////////////////////////////////////////////////////////////////////
  virtual bool lock() = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief checks whether the guarded resource is locked
  /// @returns true if resource is already locked
  ////////////////////////////////////////////////////////////////////////////
  virtual bool is_locked() const = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief unlocks the guarded resource
  ////////////////////////////////////////////////////////////////////////////
  virtual void unlock() NOEXCEPT = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief tries to lock the guarded resource within the specified amount of
  ///        time
  /// @param[in] wait_timeout timeout between different locking attempts
  /// @returns true on success
  ////////////////////////////////////////////////////////////////////////////
  bool try_lock(size_t wait_timeout = 1000);
}; // unique_lock

//////////////////////////////////////////////////////////////////////////////
/// @struct directory 
/// @brief represents a flat directory of write once/read many files
//////////////////////////////////////////////////////////////////////////////
struct IRESEARCH_API directory 
  : public util::attributes_provider, 
    private util::noncopyable {
  typedef std::vector<std::string> files;
  typedef std::function<bool(std::string& name)> visitor_f;

  DECLARE_PTR(directory);
  DECLARE_FACTORY(directory);

  ////////////////////////////////////////////////////////////////////////////
  /// @brief destructor 
  ////////////////////////////////////////////////////////////////////////////
  virtual ~directory();

  ////////////////////////////////////////////////////////////////////////////
  /// @brief closes directory
  ////////////////////////////////////////////////////////////////////////////
  virtual void close() = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief returns the list of existsing files
  /// @param[out] names the list where file names will be stored
  /// @returns true on success
  ////////////////////////////////////////////////////////////////////////////
  virtual bool list(files& names) const = 0;

  virtual bool visit(const visitor_f& visitor) const = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief check whether the file specified by the given name exists
  /// @param[in] name name of the file
  /// @returns true if file already exists 
  ////////////////////////////////////////////////////////////////////////////
  virtual bool exists(const std::string& name) const = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief returns modification time of the file specified by the given name
  /// @param[in] name name of the file
  /// @returns true if file already exists 
  ////////////////////////////////////////////////////////////////////////////
  virtual std::time_t mtime(const std::string& name) const = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief removes the file specified by the given name from directory
  /// @param[in] name name of the file
  /// @returns true if file has been removed
  ////////////////////////////////////////////////////////////////////////////
  virtual bool remove(const std::string& name) = 0;
  
  ////////////////////////////////////////////////////////////////////////////
  /// @brief renames the 'src' file to 'dst'
  /// @param[in] src initial name of the file
  /// @param[in] dst final name of the file
  ////////////////////////////////////////////////////////////////////////////
  virtual void rename(
    const std::string& src,
    const std::string& dst) = 0;
  
  ////////////////////////////////////////////////////////////////////////////
  /// @brief returns the length of the file specified by the given name
  /// @param[in] name name of the file
  /// @returns length of the file specified by the name
  ////////////////////////////////////////////////////////////////////////////
  virtual int64_t length(const std::string& name) const = 0;
  
  ////////////////////////////////////////////////////////////////////////////
  /// @brief ensures that all modification have been sucessfully persisted
  /// @param[in] name name of the file
  ////////////////////////////////////////////////////////////////////////////
  virtual void sync(const std::string& name) = 0;
  
  ////////////////////////////////////////////////////////////////////////////
  /// @brief creates an index level lock with the specified name 
  /// @param[in] name name of the lock
  /// @returns lock hande
  ////////////////////////////////////////////////////////////////////////////
  virtual index_lock::ptr make_lock(const std::string& name) = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief opens output stream associated with the file
  /// @param[in] name name of the file to open
  /// @returns output stream associated with the file with the specified name
  ////////////////////////////////////////////////////////////////////////////
  virtual index_output::ptr create(const std::string& name) = 0;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief opens input stream associated with the existing file
  /// @param[in] name   name of the file to open
  /// @returns input stream associated with the file with the specified name
  ////////////////////////////////////////////////////////////////////////////
  virtual index_input::ptr open(const std::string& name) const = 0;
}; // directory
  
NS_END

#endif
