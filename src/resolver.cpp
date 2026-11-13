// SPDX-License-Identifier: Apache-2.0
// Copyright 2023 The Foundry Visionmongers Ltd

#include <atomic>
#include <stdexcept>

#include <pxr/base/arch/threads.h>
#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/defaultResolver.h>
#include <pxr/usd/ar/defineResolver.h>

#include <openassetio/Context.hpp>
#include <openassetio/access.hpp>
#include <openassetio/hostApi/HostInterface.hpp>
#include <openassetio/hostApi/Manager.hpp>
#include <openassetio/hostApi/ManagerFactory.hpp>
#include <openassetio/log/LoggerInterface.hpp>
#include <openassetio/log/SeverityFilter.hpp>
#include <openassetio/pluginSystem/CppPluginSystemManagerImplementationFactory.hpp>
#include <openassetio/pluginSystem/HybridPluginSystemManagerImplementationFactory.hpp>
#include <openassetio/python/hostApi.hpp>
#include <openassetio/utils/path.hpp>

#include <openassetio_mediacreation/traits/content/LocatableContentTrait.hpp>

// NOLINTNEXTLINE
PXR_NAMESPACE_USING_DIRECTIVE
PXR_NAMESPACE_OPEN_SCOPE
TF_DEBUG_CODES(OPENASSETIO_RESOLVER)
PXR_NAMESPACE_CLOSE_SCOPE

namespace {
/*
 * OpenAssetIO LoggerInterface implementation
 *
 * Converter logger from OpenAssetIO log framing to USD log outputs.
 */
class UsdOpenAssetIOResolverLogger final : public openassetio::log::LoggerInterface {
 public:
  void log(const Severity severity, const openassetio::Str &message) override {
    switch (severity) {
      case Severity::kCritical:
        TF_ERROR(TfDiagnosticType::TF_DIAGNOSTIC_FATAL_ERROR_TYPE, message);
        break;
      case Severity::kDebug:
      case Severity::kDebugApi:
        TF_DEBUG(OPENASSETIO_RESOLVER).Msg(message + "\n");
        break;
      case Severity::kError:
        TF_ERROR(TfDiagnosticType::TF_DIAGNOSTIC_NONFATAL_ERROR_TYPE, message);
        break;
      case Severity::kInfo:
      case Severity::kProgress:
        TF_INFO(OPENASSETIO_RESOLVER).Msg(message + "\n");
        break;
      case Severity::kWarning:
        TF_WARN(TfDiagnosticType::TF_DIAGNOSTIC_WARNING_TYPE, message);
        break;
    }
  }
};

/**
 * OpenAssetIO HostInterface implementation
 *
 * This identifies the Ar2 plugin uniquely should the manager plugin
 * wish to adapt its behaviour.
 */
class UsdOpenAssetIOHostInterface final : public openassetio::hostApi::HostInterface {
 public:
  [[nodiscard]] openassetio::Identifier identifier() const override {
    return "org.openassetio.usdresolver";
  }

  [[nodiscard]] openassetio::Str displayName() const override {
    return "OpenAssetIO USD Resolver";
  }
};

/**
 * The process-wide logger, available before the manager is.
 */
const openassetio::log::LoggerInterfacePtr &sharedLogger() {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  static const auto *const kLogger = new openassetio::log::LoggerInterfacePtr{
      openassetio::log::SeverityFilter::make(std::make_shared<UsdOpenAssetIOResolverLogger>())};
  return *kLogger;
}

/**
 * The OpenAssetIO objects resolver instances talk to.
 */
struct OpenAssetIOSession {
  openassetio::log::LoggerInterfacePtr logger;
  openassetio::hostApi::ManagerPtr manager;
  openassetio::ContextConstPtr context;
};

OpenAssetIOSession createOpenAssetIOSession() {
  OpenAssetIOSession session;
  session.logger = sharedLogger();
  // Python plugin system.
  const auto pythonManagerImplementationFactory =
      openassetio::python::hostApi::createPythonPluginSystemManagerImplementationFactory(
          session.logger);
  // C++ plugin system.
  const auto cppManagerImplementationFactory =
      openassetio::pluginSystem::CppPluginSystemManagerImplementationFactory::make(session.logger);
  // Combined Python/C++ plugin system. Note: C++ is listed first so
  // has priority.
  const auto hybridManagerImplementationFactory =
      openassetio::pluginSystem::HybridPluginSystemManagerImplementationFactory::make(
          {cppManagerImplementationFactory, pythonManagerImplementationFactory}, session.logger);
  const auto hostInterface = std::make_shared<UsdOpenAssetIOHostInterface>();
  // Find the plugin configured using the OPENASSETIO_DEFAULT_CONFIG
  // file, initialise it, and create the host-facing Manager
  // middleware for us to communicate with it.
  session.manager = openassetio::hostApi::ManagerFactory::defaultManagerForInterface(
      hostInterface, hybridManagerImplementationFactory, session.logger);
  if (!session.manager) {
    throw std::invalid_argument{
        "No default manager configured, " +
        openassetio::hostApi::ManagerFactory::kDefaultManagerConfigEnvVarName};
  }
  // This USD plugin is useless unless the manager plugin has the
  // basic ability to `resolve(...)`.
  if (!session.manager->hasCapability(openassetio::hostApi::Manager::Capability::kResolution)) {
    throw std::invalid_argument{session.manager->displayName() +
                                " is not capable of resolving entity references"};
  }
  session.context = session.manager->createContext();
  return session;
}

/**
 * The process-wide OpenAssetIO session, shared by all resolver
 * instances and created on first use.
 *
 * USD creates resolvers on whichever thread first needs one, including
 * worker threads (e.g. while registering shader definitions at host
 * startup), and may construct several concurrently and discard all but
 * one. Starting a manager plugin system there can import Python on a
 * worker thread, which some hosts do not survive. So resolver instances
 * do no OpenAssetIO work when constructed; they share one session,
 * initialized once when first needed (a failed initialization is
 * retried on next use). It is intentionally never destroyed, so that it
 * is not torn down during process exit after the Python interpreter.
 */
const OpenAssetIOSession &sharedOpenAssetIOSession() {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  static const auto *const kSession = new OpenAssetIOSession{createOpenAssetIOSession()};
  return *kSession;
}

}  // namespace

// ---------------------------------------------------------------------

/*
 * Ar Resolver Implementation
 */
class UsdOpenAssetIOResolver final : public PXR_NS::ArDefaultResolver {
 public:
  UsdOpenAssetIOResolver() : logger_{sharedLogger()} {}

  ~UsdOpenAssetIOResolver() override = default;

 protected:
  void _BindContext(const ArResolverContext &context, VtValue *bindingData) override {
    ArDefaultResolver::_BindContext(context, bindingData);
    // Stages bind their context on the opening thread before composition
    // fans out to worker threads. Starting the session here, once, on the
    // main thread keeps manager initialization off worker threads.
    // A failure here is only a warning: documents without entity
    // references must still open, and resolving one reports the error.
    static std::atomic<bool> attempted{false};
    if (ArchIsMainThread() && !attempted.exchange(true)) {
      try {
        static_cast<void>(session());
      } catch (const std::exception &exc) {
        logger_->warning(std::string{"OpenAssetIO manager unavailable: "} + exc.what());
      } catch (...) {
        logger_->warning("OpenAssetIO manager unavailable: unknown non-exception type caught");
      }
    }
  }

  [[nodiscard]] std::string _CreateIdentifier(
      const std::string &assetPath, const ArResolvedPath &anchorAssetPath) const override {
    return catchAndLogExceptions(
        [&] {
          std::string identifier;
          if (session().manager->isEntityReferenceString(assetPath)) {
            identifier = assetPath;
          } else {
            identifier = ArDefaultResolver::_CreateIdentifier(assetPath, anchorAssetPath);
          }
          return identifier;
        },
        TF_FUNC_NAME());
  }

  [[nodiscard]] ArResolvedPath _Resolve(const std::string &assetPath) const override {
    return catchAndLogExceptions(
        [&] {
          if (const auto entityReference =
                  session().manager->createEntityReferenceIfValid(assetPath)) {
            return ArResolvedPath{resolveToPath(*entityReference)};
          }
          return ArDefaultResolver::_Resolve(assetPath);
        },
        TF_FUNC_NAME());
  }

  [[nodiscard]] std::string _CreateIdentifierForNewAsset(
      const std::string &assetPath, const ArResolvedPath &anchorAssetPath) const override {
    return catchAndLogExceptions(
        [&] {
          if (session().manager->isEntityReferenceString(assetPath)) {
            std::string message =
                "Writes to OpenAssetIO entity references are not currently supported ";
            message += assetPath;
            logger_->critical(message);
            return std::string{};
          }
          return ArDefaultResolver::_CreateIdentifierForNewAsset(assetPath, anchorAssetPath);
        },
        TF_FUNC_NAME());
  }

  [[nodiscard]] ArResolvedPath _ResolveForNewAsset(const std::string &assetPath) const override {
    return catchAndLogExceptions(
        [&] {
          if (session().manager->isEntityReferenceString(assetPath)) {
            std::string message =
                "Writes to OpenAssetIO entity references are not currently supported ";
            message += assetPath;
            logger_->critical(message);
            return ArResolvedPath{};
          }
          return ArDefaultResolver::_ResolveForNewAsset(assetPath);
        },
        TF_FUNC_NAME());
  }

  [[nodiscard]] std::string _GetExtension(const std::string &assetPath) const override {
    return ArDefaultResolver::_GetExtension(assetPath);
  }

  [[nodiscard]] ArAssetInfo _GetAssetInfo(const std::string &assetPath,
                                          const ArResolvedPath &resolvedPath) const override {
    return ArDefaultResolver::_GetAssetInfo(assetPath, resolvedPath);
  }

  [[nodiscard]] ArTimestamp _GetModificationTimestamp(
      const std::string &assetPath, const ArResolvedPath &resolvedPath) const override {
    return ArDefaultResolver::_GetModificationTimestamp(assetPath, resolvedPath);
  }

  [[nodiscard]] std::shared_ptr<ArAsset> _OpenAsset(
      const ArResolvedPath &resolvedPath) const override {
    return ArDefaultResolver::_OpenAsset(resolvedPath);
  }

  [[nodiscard]] bool _CanWriteAssetToPath(const ArResolvedPath &resolvedPath,
                                          std::string *whyNot) const override {
    return ArDefaultResolver::_CanWriteAssetToPath(resolvedPath, whyNot);
  }

  [[nodiscard]] std::shared_ptr<ArWritableAsset> _OpenAssetForWrite(
      const ArResolvedPath &resolvedPath, const WriteMode writeMode) const override {
    return ArDefaultResolver::_OpenAssetForWrite(resolvedPath, writeMode);
  }

 private:
  /**
   * The shared OpenAssetIO session, started on first use.
   */
  [[nodiscard]] static const OpenAssetIOSession &session() { return sharedOpenAssetIOSession(); }

  /**
   * Retrieve the resolved file path of an entity reference.
   *
   * This will resolve the "locateableContent" trait of the entity and
   * return the "location" property of it, converted from a URL to a
   * file path.
   *
   * @param entityReference The reference to resolve.
   * @return Resolved file path.
   */
  [[nodiscard]] std::string resolveToPath(
      const openassetio::EntityReference &entityReference) const {
    using openassetio::access::ResolveAccess;
    using openassetio_mediacreation::traits::content::LocatableContentTrait_v1;

    // Resolve the locatable content trait, this will provide a URL
    // that points to the final content
    const OpenAssetIOSession &openAssetIO = session();
    const openassetio::trait::TraitsDataPtr traitsData =
        openAssetIO.manager->resolve(entityReference, {LocatableContentTrait_v1::kId},
                                     ResolveAccess::kRead, openAssetIO.context);

    const std::optional<openassetio::Str> url = LocatableContentTrait_v1(traitsData).getLocation();
    if (!url) {
      throw std::invalid_argument{"Entity reference does not have a location: " +
                                  entityReference.toString()};
    }
    // OpenAssetIO is URL based, but we need a path. Note: will throw if
    // the URL is not valid.
    return fileUrlPathConverter_.pathFromUrl(*url);
  }

  /**
   * Decorator to stop propagation of all exceptions.
   *
   * Exceptions occurring within the wrapped callable will be caught and
   * logged, and a default-constructed value (of the same type as the
   * callable's return type) returned instead of propagating the
   * exception.
   *
   * The callable must take no arguments (but may return a value).
   *
   * This is needed since USD reacts badly if an exception escapes an Ar
   * plugin (segfault, sigabrt).
   *
   * @tparam Fn Callable type.
   * @param func Callable instance.
   * @param name Name of function/process that caused the exception, to
   * append to log messages.
   * @return Result of callable if no exception occurred, otherwise a
   * default-constructed object.
   */
  template <typename Fn>
  auto catchAndLogExceptions(Fn &&func, const std::string_view name) const
      -> std::invoke_result_t<Fn> {
    try {
      return func();
    } catch (const std::exception &exc) {
      std::string msg = "OpenAssetIO error in ";
      msg += name;
      msg += ": ";
      msg += exc.what();
      logger_->critical(msg);
    } catch (...) {
      std::string msg = "OpenAssetIO error in ";
      msg += name;
      msg += ": unknown non-exception type caught";
      logger_->critical(msg);
    }
    return {};
  }

  openassetio::log::LoggerInterfacePtr logger_;
  openassetio::utils::FileUrlPathConverter fileUrlPathConverter_;
};

PXR_NAMESPACE_OPEN_SCOPE
AR_DEFINE_RESOLVER(UsdOpenAssetIOResolver, ArResolver)
PXR_NAMESPACE_CLOSE_SCOPE
