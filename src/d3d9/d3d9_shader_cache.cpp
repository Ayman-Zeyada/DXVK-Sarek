#include <iomanip>
#include <algorithm>
#include <cstring>
#include <version.h>

#include "d3d9_shader_cache.h"

#include "../util/log/log.h"

namespace dxvk {

  D3D9ShaderCache::Instance D3D9ShaderCache::s_instance;


  D3D9ShaderCache::D3D9ShaderCache()
  : m_filePaths(getFilePaths()) {

  }


  D3D9ShaderCache::~D3D9ShaderCache() {

  }


  bool D3D9ShaderCache::loadShader(
    const std::string&          name,
          VkShaderStageFlagBits stage,
    const DxsoOptions&          options,
    const D3D9ConstantLayout&   layout,
          D3D9CachedShaderData& shader) {
    std::unique_lock lock(m_mutex);

    if (!ensureInitializedLocked())
      return false;

    LutKey key;
    key.name = name;
    key.stage = uint32_t(stage);
    key.options = options;
    key.layout = layout;

    return loadShaderLocked(key, shader);
  }


  void D3D9ShaderCache::storeShader(
    const std::string&          name,
          VkShaderStageFlagBits stage,
    const DxsoOptions&          options,
    const D3D9ConstantLayout&   layout,
    const D3D9CommonShader&     shader) {
    std::unique_lock lock(m_mutex);

    if (!ensureInitializedLocked())
      return;

    LutKey key;
    key.name = name;
    key.stage = uint32_t(stage);
    key.options = options;
    key.layout = layout;

    storeShaderLocked(key, shader);
  }


  Rc<D3D9ShaderCache> D3D9ShaderCache::getInstance() {
    std::lock_guard lock(s_instance.mutex);

    if (!s_instance.instance)
      s_instance.instance = new D3D9ShaderCache();

    return s_instance.instance;
  }


  bool D3D9ShaderCache::ensureInitializedLocked() {
    if (m_status != Status::Uninitialized)
      return m_status == Status::OpenReadWrite;

    m_status = tryInitializeLocked()
      ? Status::OpenReadWrite
      : Status::CacheDisabled;

    return m_status == Status::OpenReadWrite;
  }


  bool D3D9ShaderCache::tryInitializeLocked() {
    if (m_filePaths.directory.empty() || m_filePaths.binFile.empty() || m_filePaths.lutFile.empty()) {
      Logger::warn("No path found for D3D9 shader cache, consider setting DXVK_SHADER_CACHE_PATH.");
      return false;
    }

    if (openReadWriteLocked()) {
      if (parseLutLocked())
        return true;
    }

    return openWriteOnlyLocked();
  }


  bool D3D9ShaderCache::openReadWriteLocked() {
    auto path = m_filePaths.directory + env::PlatformDirSlash;

    auto flags = util::FileFlags(
      util::FileFlag::AllowRead,
      util::FileFlag::AllowWrite,
      util::FileFlag::Exclusive);

    m_binFile = util::File();
    m_lutFile = util::File();

    m_binFile.open(path + m_filePaths.binFile, flags);
    m_lutFile.open(path + m_filePaths.lutFile, flags);

    if (!m_binFile || !m_lutFile)
      return false;

    Logger::info(str::format("Found D3D9 cache file: ", path + m_filePaths.binFile));
    return true;
  }


  bool D3D9ShaderCache::openWriteOnlyLocked() {
    auto path = m_filePaths.directory + env::PlatformDirSlash;

    auto flags = util::FileFlags(
      util::FileFlag::AllowRead,
      util::FileFlag::AllowWrite,
      util::FileFlag::Truncate,
      util::FileFlag::Exclusive);

    m_binFile = util::File();
    m_lutFile = util::File();

    m_binFile.open(path + m_filePaths.binFile, flags);
    m_lutFile.open(path + m_filePaths.lutFile, flags);

    if (!m_binFile || !m_lutFile) {
      if (!env::createDirectory(m_filePaths.directory)) {
        Logger::warn(str::format("Failed to create directory: ", m_filePaths.directory));
        return false;
      }

      m_binFile.open(path + m_filePaths.binFile, flags);
      m_lutFile.open(path + m_filePaths.lutFile, flags);
    }

    if (!m_binFile)
      Logger::warn(str::format("Failed to create ", path + m_filePaths.binFile, ", disabling D3D9 cache"));

    if (!m_lutFile)
      Logger::warn(str::format("Failed to create ", path + m_filePaths.lutFile, ", disabling D3D9 cache"));

    if (!m_binFile || !m_lutFile)
      return false;

    if (!writeHeader(m_lutFile)) {
      Logger::warn(str::format("Failed to write D3D9 cache header: ", path + m_filePaths.lutFile));
      return false;
    }

    m_lut.clear();

    Logger::info(str::format("Created D3D9 cache file: ", path + m_filePaths.binFile));
    return true;
  }


  bool D3D9ShaderCache::parseLutLocked() {
    size_t offset = 0u;
    size_t size = m_lutFile.size();

    if (!readHeader(m_lutFile, offset))
      return false;

    while (offset < size) {
      LutKey key;
      LutEntry entry;

      if (!readLutKey(m_lutFile, offset, key)
       || !read(m_lutFile, offset, entry)) {
        Logger::warn("Failed to parse D3D9 shader cache look-up table.");
        return false;
      }

      m_lut.insert_or_assign(key, entry);
    }

    return true;
  }


  bool D3D9ShaderCache::loadShaderLocked(
    const LutKey&             key,
          D3D9CachedShaderData& shader) {
    auto entry = m_lut.find(key);

    if (entry == m_lut.end())
      return false;

    size_t offset = size_t(entry->second.offset);
    if (!readShaderBinary(m_binFile, offset, shader)) {
      Logger::warn(str::format("Failed to load cached D3D9 shader ", key.name));
      return false;
    }

    return true;
  }


  bool D3D9ShaderCache::storeShaderLocked(
    const LutKey&           key,
    const D3D9CommonShader& shader) {
    if (m_lut.find(key) != m_lut.end())
      return true;

    LutEntry entry;

    if (!writeShaderBinary(m_binFile, shader, entry)
     || !writeLutKey(m_lutFile, key)
     || !write(m_lutFile, entry)) {
      Logger::err("Failed to write D3D9 shader cache file.");
      m_status = Status::CacheDisabled;
      return false;
    }

    m_binFile.flush();
    m_lutFile.flush();

    m_lut.insert_or_assign(key, entry);
    return true;
  }


  D3D9ShaderCache::FilePaths D3D9ShaderCache::getFilePaths() {
    std::string cachePath = env::getEnvVar("DXVK_SHADER_CACHE_PATH");

    if (cachePath.empty()) {
      #ifdef _WIN32
      cachePath = env::getEnvVar("LOCALAPPDATA");
      #endif

      if (cachePath.empty())
        cachePath = env::getEnvVar("XDG_CACHE_HOME");

      if (cachePath.empty()) {
        cachePath = env::getEnvVar("HOME");

        if (!cachePath.empty()) {
          cachePath += env::PlatformDirSlash;
          cachePath += ".cache";
        }
      }

      if (!cachePath.empty()) {
        cachePath += env::PlatformDirSlash;
        cachePath += "dxvk";
      }
    }

    if (cachePath.empty())
      return FilePaths();

    std::string exePath = env::getExePath();

    if (exePath.empty())
      return FilePaths();

    size_t pathStart = exePath.find_last_of(env::PlatformDirSlash);

    if (pathStart != std::string::npos)
      pathStart = exePath.find_last_of(env::PlatformDirSlash, pathStart);

    if (pathStart == std::string::npos)
      pathStart = 0u;

    uint64_t hash = bit::fnv1a_init();

    for (size_t i = pathStart; i < exePath.size(); i++)
      hash = bit::fnv1a_iter(hash, uint8_t(exePath[i]));

    std::string baseName = str::format(std::hex, std::setw(16u), std::setfill('0'), hash);

    FilePaths paths;
    paths.directory = cachePath;
    paths.lutFile = baseName + ".d3d9.dxvk.lut";
    paths.binFile = baseName + ".d3d9.dxvk.bin";
    return paths;
  }


  bool D3D9ShaderCache::writeHeader(util::File& stream) {
    static constexpr std::array<char, 4u> s_magic = { 'D', '9', 'V', 'K' };

    return stream.append(s_magic.size(), s_magic.data())
        && writeString(stream, DXVK_VERSION);
  }


  bool D3D9ShaderCache::writeString(util::File& stream, const std::string& string) {
    return write(stream, uint16_t(string.size()))
        && stream.append(string.size(), string.data());
  }


  bool D3D9ShaderCache::writeLutKey(util::File& stream, const LutKey& key) {
    return writeString(stream, key.name)
        && write(stream, key.stage)
        && write(stream, uint32_t(key.options.d3d9FloatEmulation))
        && write(stream, key.options.forceSamplerTypeSpecConstants)
        && write(stream, key.options.forceSampleRateShading)
        && write(stream, key.options.vertexFloatConstantBufferAsSSBO)
        && write(stream, key.options.sincosEmulation)
        && write(stream, key.options.enableClipDistance)
        && write(stream, key.layout);
  }


  bool D3D9ShaderCache::writeShaderBinary(
          util::File&          stream,
    const D3D9CommonShader&    shader,
          LutEntry&            entry) {
    auto data = shader.getCacheData();

    if (data.spirv.empty())
      return false;

    entry.offset = stream.size();

    return write(stream, uint32_t(data.info.type()))
        && write(stream, data.info.majorVersion())
        && write(stream, data.info.minorVersion())
        && write(stream, data.isgn)
        && write(stream, data.meta)
        && write(stream, data.usedSamplers)
        && write(stream, data.usedRTs)
        && write(stream, data.textureTypes)
        && write(stream, data.maxDefinedFloatConst)
        && write(stream, data.maxDefinedIntConst)
        && write(stream, data.maxDefinedBoolConst)
        && writeVector(stream, data.constants)
        && writeVector(stream, data.bindings)
        && write(stream, data.flatShadingInputs)
        && write(stream, data.sharedPushData)
        && write(stream, data.localPushData)
        && write(stream, data.samplerHeap)
        && write(stream, data.xfbRasterizedStream)
        && write(stream, data.patchVertexCount)
        && writeString(stream, data.debugName)
        && writeVector(stream, data.spirv);
  }


  bool D3D9ShaderCache::readString(util::File& stream, size_t& offset, std::string& string) {
    uint16_t length = 0u;

    if (!read(stream, offset, length))
      return false;

    string.resize(length);
    if (length == 0u)
      return true;

    bool result = stream.read(offset, length, string.data());
    offset += length;
    return result;
  }


  bool D3D9ShaderCache::readHeader(util::File& stream, size_t& offset) {
    std::array<char, 4u> magic = { };
    std::string version;

    if (!stream.read(offset, magic.size(), magic.data())) {
      Logger::warn("Failed to parse D3D9 cache file header.");
      return false;
    }

    offset += magic.size();

    if (magic != std::array<char, 4u>{ 'D', '9', 'V', 'K' }) {
      Logger::warn("Unexpected D3D9 shader cache file header.");
      return false;
    }

    if (!readString(stream, offset, version)) {
      Logger::warn("Failed to parse D3D9 cache file version.");
      return false;
    }

    if (version != DXVK_VERSION) {
      Logger::warn(str::format("D3D9 cache was created with DXVK version ", version,
        ", but current version is ", DXVK_VERSION, ". Discarding old cache."));
      return false;
    }

    return true;
  }


  bool D3D9ShaderCache::readLutKey(util::File& stream, size_t& offset, LutKey& key) {
    uint32_t floatEmulation = 0u;

    if (!readString(stream, offset, key.name)
     || !read(stream, offset, key.stage)
     || !read(stream, offset, floatEmulation)
     || !read(stream, offset, key.options.forceSamplerTypeSpecConstants)
     || !read(stream, offset, key.options.forceSampleRateShading)
     || !read(stream, offset, key.options.vertexFloatConstantBufferAsSSBO)
     || !read(stream, offset, key.options.sincosEmulation)
     || !read(stream, offset, key.options.enableClipDistance)
     || !read(stream, offset, key.layout))
      return false;

    key.options.d3d9FloatEmulation = D3D9FloatEmulation(floatEmulation);
    return true;
  }


  bool D3D9ShaderCache::readShaderBinary(
          util::File&            stream,
          size_t&                offset,
          D3D9CachedShaderData&  shader) {
    uint32_t programType = 0u;
    uint32_t majorVersion = 0u;
    uint32_t minorVersion = 0u;

    if (!read(stream, offset, programType)
     || !read(stream, offset, majorVersion)
     || !read(stream, offset, minorVersion)
     || !read(stream, offset, shader.isgn)
     || !read(stream, offset, shader.meta)
     || !read(stream, offset, shader.usedSamplers)
     || !read(stream, offset, shader.usedRTs)
     || !read(stream, offset, shader.textureTypes)
     || !read(stream, offset, shader.maxDefinedFloatConst)
     || !read(stream, offset, shader.maxDefinedIntConst)
     || !read(stream, offset, shader.maxDefinedBoolConst)
     || !readVector(stream, offset, shader.constants)
     || !readVector(stream, offset, shader.bindings)
     || !read(stream, offset, shader.flatShadingInputs)
     || !read(stream, offset, shader.sharedPushData)
     || !read(stream, offset, shader.localPushData)
     || !read(stream, offset, shader.samplerHeap)
     || !read(stream, offset, shader.xfbRasterizedStream)
     || !read(stream, offset, shader.patchVertexCount)
     || !readString(stream, offset, shader.debugName)
     || !readVector(stream, offset, shader.spirv))
      return false;

    shader.info = DxsoProgramInfo(DxsoProgramType(programType), minorVersion, majorVersion);
    return !shader.spirv.empty();
  }


  bool D3D9ShaderCache::optionsEq(const DxsoOptions& a, const DxsoOptions& b) {
    return a.d3d9FloatEmulation == b.d3d9FloatEmulation
        && a.forceSamplerTypeSpecConstants == b.forceSamplerTypeSpecConstants
        && a.forceSampleRateShading == b.forceSampleRateShading
        && a.vertexFloatConstantBufferAsSSBO == b.vertexFloatConstantBufferAsSSBO
        && a.sincosEmulation == b.sincosEmulation
        && a.enableClipDistance == b.enableClipDistance;
  }


  size_t D3D9ShaderCache::optionsHash(const DxsoOptions& options) {
    DxvkHashState hash;
    hash.add(uint32_t(options.d3d9FloatEmulation));
    hash.add(options.forceSamplerTypeSpecConstants);
    hash.add(options.forceSampleRateShading);
    hash.add(options.vertexFloatConstantBufferAsSSBO);
    hash.add(options.sincosEmulation);
    hash.add(options.enableClipDistance);
    return hash;
  }
  size_t D3D9ShaderCache::LutKey::hash() const {
    DxvkHashState hash;
    hash.add(std::hash<std::string>{}(name));
    hash.add(stage);
    hash.add(D3D9ShaderCache::optionsHash(options));
    hash.add(layout.floatCount);
    hash.add(layout.intCount);
    hash.add(layout.boolCount);
    hash.add(layout.bitmaskCount);
    return hash;
  }


  bool D3D9ShaderCache::LutKey::eq(const LutKey& other) const {
    return name == other.name
        && stage == other.stage
        && D3D9ShaderCache::optionsEq(options, other.options)
        && layout.floatCount == other.layout.floatCount
        && layout.intCount == other.layout.intCount
        && layout.boolCount == other.layout.boolCount
        && layout.bitmaskCount == other.layout.bitmaskCount;
  }

}
