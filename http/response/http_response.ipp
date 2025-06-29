namespace WFX::Http {

using namespace WFX::Utils;

template<typename T>
void HttpResponse::SendText(T&& text)
{
    static_assert(std::is_same_v<std::decay_t<T>, std::string>,
                  "SendText only accepts std::string (copy or move)");

    auto& logger = Logger::GetInstance();

    if(!body.empty())
        logger.Fatal("[HttpResponse]: Calling 'SendText' multiple times or after another Send... function is not allowed");
    
    if(isFileOperation)
        logger.Fatal("[HttpResponse]: Cannot mix file/text responses");

    body = std::forward<T>(text);

    headers.SetHeader("Content-Length", UInt64ToStr(body.size()));
    headers.SetHeader("Content-Type", "text/plain");
}

template<typename T>
void HttpResponse::SendJson(T&& json)
{
    static_assert(std::is_same_v<std::decay_t<T>, Json>,
                  "SendJson only accepts WFX::Http::Json (copy or move)");
    
    auto& logger = Logger::GetInstance();

    if(!body.empty())
        logger.Fatal("[HttpResponse]: Calling 'SendJson' multiple times or after another Send... function is not allowed");

    if(isFileOperation)
        logger.Fatal("[HttpResponse]: Cannot mix file/json responses");

    body = std::forward<T>(json).dump();

    headers.SetHeader("Content-Length", UInt64ToStr(body.size()));
    headers.SetHeader("Content-Type", "application/json");
}

template<typename T>
void HttpResponse::SendFile(T&& path)
{
    static_assert(std::is_same_v<std::decay_t<T>, std::string>,
                  "SendFile only accepts std::string (copy or move)");
    
    auto& logger = Logger::GetInstance();
    auto& fs     = FileSystem::GetFileSystem();

    if(!body.empty())
        logger.Fatal("[HttpResponse]: Calling 'SendFile' multiple times or after another Send... function is not allowed");

    if(!fs.FileExists(path))
        logger.Fatal("[HttpResponse]: In 'SendFile', File not found: ", path);

    isFileOperation = true;
    body = std::forward<T>(path);
    
    // Headers
    headers.SetHeader("Content-Length", UInt64ToStr(fs.GetFileSize(path)));
    headers.SetHeader("Content-Type", std::string(MimeDetector::DetectMimeFromExt(body)));
}

} // namespace WFX::Http