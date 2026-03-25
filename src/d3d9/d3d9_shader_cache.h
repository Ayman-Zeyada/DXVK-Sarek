#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../dxso/dxso_options.h"
#include "../dxvk/dxvk_hash.h"
#include "../util/rc/util_rc.h"
#include "../util/thread.h"
#include "../util/util_env.h"
#include "../util/util_file.h"

#include "d3d9_constant_layout.h"
#include "d3d9_shader.h"

namespace dxvk {

  class D3D9ShaderCache : public RcObject {

  public:

    ~D3D9ShaderCache();

    bool loadShader(
      const std::string&          name,
            VkShaderStageFlagBits stage,
      const DxsoOptions&          options,
      const D3D9ConstantLayout&   layout,
            D3D9CachedShaderData& shader);

    void storeShader(
      const std::string&          name,
            VkShaderStageFlagBits stage,
      const DxsoOptions&          options,
      const D3D9ConstantLayout&   layout,
      const D3D9CommonShader&     shader);

    static Rc<D3D9ShaderCache> getInstance();

  private:

    struct FilePaths {
      std::string directory;
      std::string lutFile;
      std::string binFile;
    };

    struct Instance {
      dxvk::mutex         mutex;
      Rc<D3D9ShaderCache> instance;
    };

    struct LutKey {
      std::string         name;
      uint32_t            stage = 0u;
      DxsoOptions         options = { };
      D3D9ConstantLayout  layout = { };

      size_t hash() const;
      bool eq(const LutKey& other) const;
    };

    struct LutEntry {
      uint64_t offset = 0u;
    };

    enum class Status : uint32_t {
      Uninitialized = 0u,
      CacheDisabled = 1u,
      OpenReadWrite = 2u,
    };

    static Instance s_instance;

    FilePaths    m_filePaths;
    dxvk::mutex  m_mutex;
    util::File   m_lutFile;
    util::File   m_binFile;
    Status       m_status = Status::Uninitialized;

    std::unordered_map<LutKey, LutEntry, DxvkHash, DxvkEq> m_lut;

    D3D9ShaderCache();

    bool ensureInitializedLocked();
    bool tryInitializeLocked();
    bool openReadWriteLocked();
    bool openWriteOnlyLocked();
    bool parseLutLocked();

    bool loadShaderLocked(
      const LutKey&             key,
            D3D9CachedShaderData& shader);

    bool storeShaderLocked(
      const LutKey&             key,
      const D3D9CommonShader&   shader);

    static FilePaths getFilePaths();

    static bool writeHeader(util::File& stream);
    static bool writeString(util::File& stream, const std::string& string);
    static bool writeLutKey(util::File& stream, const LutKey& key);
    static bool writeShaderBinary(util::File& stream, const D3D9CommonShader& shader, LutEntry& entry);

    static bool readString(util::File& stream, size_t& offset, std::string& string);
    static bool readHeader(util::File& stream, size_t& offset);
    static bool readLutKey(util::File& stream, size_t& offset, LutKey& key);
    static bool readShaderBinary(util::File& stream, size_t& offset, D3D9CachedShaderData& shader);

    static bool optionsEq(const DxsoOptions& a, const DxsoOptions& b);
    static size_t optionsHash(const DxsoOptions& options);

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool write(util::File& stream, const T& data) {
      return stream.append(sizeof(T), &data);
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool read(util::File& stream, size_t& offset, T& data) {
      bool result = stream.read(offset, sizeof(T), &data);
      offset += sizeof(T);
      return result;
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool writeVector(util::File& stream, const std::vector<T>& data) {
      return write(stream, uint32_t(data.size()))
          && (data.empty() || stream.append(data.size() * sizeof(T), data.data()));
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool readVector(util::File& stream, size_t& offset, std::vector<T>& data) {
      uint32_t count = 0u;

      if (!read(stream, offset, count))
        return false;

      data.resize(count);
      if (count == 0u)
        return true;

      bool result = stream.read(offset, count * sizeof(T), data.data());
      offset += count * sizeof(T);
      return result;
    }

  };

}
