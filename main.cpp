#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <openssl/sha.h>

#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/logs/CloudWatchLogsClient.h>
#include <aws/logs/model/PutLogEventsRequest.h>
#include <aws/logs/model/InputLogEvent.h>
#include <aws/logs/model/CreateLogGroupRequest.h>
#include <aws/logs/model/CreateLogStreamRequest.h>
#include <aws/logs/model/DescribeLogStreamsRequest.h>


std::string calculateSHA256(const std::string& fileName)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];

    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    std::ifstream file(fileName, std::ios::binary);

    if (!file)
    {
        return "";
    }

    char buffer[4096];

    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
    {
        SHA256_Update(&sha256, buffer, file.gcount());
    }

    SHA256_Final(hash, &sha256);

    std::stringstream result;

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
        result << std::hex
               << std::setw(2)
               << std::setfill('0')
               << static_cast<int>(hash[i]);
    }

    return result.str();
}



bool fileAlreadyExists(
    Aws::S3::S3Client& s3Client,
    const Aws::String& bucketName,
    const std::string& newHash
)
{
    Aws::S3::Model::ListObjectsV2Request listRequest;
    listRequest.SetBucket(bucketName);

    auto listOutcome = s3Client.ListObjectsV2(listRequest);

    if (!listOutcome.IsSuccess())
    {
        std::cout << "Could not check for duplicates: "
                  << listOutcome.GetError().GetMessage()
                  << "\n";

        return false;
    }

    for (const auto& object : listOutcome.GetResult().GetContents())
    {
        Aws::S3::Model::HeadObjectRequest headRequest;
        headRequest.SetBucket(bucketName);
        headRequest.SetKey(object.GetKey());

        auto headOutcome = s3Client.HeadObject(headRequest);

        if (!headOutcome.IsSuccess())
        {
            continue;
        }

        auto metadata = headOutcome.GetResult().GetMetadata();
        auto hashIt = metadata.find("sha256");

        if (hashIt != metadata.end() &&
            hashIt->second == newHash)
        {
            std::cout << "Duplicate content found in S3 object: "
                      << object.GetKey()
                      << "\n";
            return true;
        }
    }

    return false;
}


void logEvent(
    Aws::CloudWatchLogs::CloudWatchLogsClient& logsClient,
    const Aws::String& message)
{
    Aws::CloudWatchLogs::Model::PutLogEventsRequest request;

    request.SetLogGroupName("/cloudvault/operations");
    request.SetLogStreamName("local-session");

    Aws::CloudWatchLogs::Model::InputLogEvent event;

    event.SetMessage(message);

    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

    event.SetTimestamp(milliseconds);

    request.AddLogEvents(event);

    auto outcome = logsClient.PutLogEvents(request);

    if (!outcome.IsSuccess())
    {
        std::cout << "CloudWatch logging failed: "
                  << outcome.GetError().GetMessage()
                  << "\n";
    }
}


int main()
{
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    {
        Aws::Client::ClientConfiguration config;
        config.region = "us-east-1";
        config.endpointOverride = "http://localhost:4566";

        Aws::Auth::AWSCredentials credentials(
            "varun",
            "varun"
        );

        Aws::S3::S3Client s3Client(
            credentials,
            config,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            false
        );

        Aws::CloudWatchLogs::CloudWatchLogsClient logsClient(
            credentials,
            config);

        const Aws::String bucketName = "cloudvault-documents";

        int choice;

        while (true)
        {
            std::cout << "\n===== CloudVault =====\n";
            std::cout << "1. Upload file\n";
            std::cout << "2. List files\n";
            std::cout << "3. Download file\n";
            std::cout << "4. Delete file\n";
            std::cout << "5. Exit\n";
            std::cout << "Enter choice: ";

            std::cin >> choice;

            // ---------------- UPLOAD ----------------
            if (choice == 1)
            {
                std::string fileName;

                std::cout << "Enter file name to upload: ";
                std::cin >> fileName;

                std::ifstream file(fileName, std::ios::binary);

                if (!file)
                {
                    std::cout << "Could not open file.\n";
                    continue;
                }

                std::string hash = calculateSHA256(fileName);
                if (hash.empty())
                {
                    std::cout << "Could not calculate file hash.\n";
                    continue;
                }
                std::cout << "SHA-256: " << hash << "\n";

                // Check whether identical content already exists in S3
                if (fileAlreadyExists(s3Client, bucketName, hash))
                {
                    std::cout << "Duplicate file detected! Upload skipped.\n";
                    continue;
                }

                std::stringstream buffer;
                buffer << file.rdbuf();

                auto inputData = Aws::MakeShared<Aws::StringStream>(
                    "UploadTag"
                );

                *inputData << buffer.str();

                Aws::S3::Model::PutObjectRequest request;

                request.SetBucket(bucketName);
                request.SetKey(fileName);
                request.SetBody(inputData);
                request.AddMetadata("sha256", hash);

                auto outcome = s3Client.PutObject(request);

                if (!outcome.IsSuccess())
                {
                    std::cout << "Upload failed: "
                              << outcome.GetError().GetMessage()
                              << "\n";
                }
                else
                {
                    std::cout << "File uploaded successfully!\n";
                    logEvent(logsClient, "File uploaded successfully: " + fileName);
                }
            }

            // ---------------- LIST ----------------
            else if (choice == 2)
            {
                Aws::S3::Model::ListObjectsV2Request request;

                request.SetBucket(bucketName);

                auto outcome = s3Client.ListObjectsV2(request);

                if (!outcome.IsSuccess())
                {
                    std::cout << "Could not list files: "
                              << outcome.GetError().GetMessage()
                              << "\n";
                }
                else
                {
                    std::cout << "\nFiles in CloudVault:\n";

                    const auto& objects = outcome.GetResult().GetContents();

                    if (objects.empty())
                    {
                        std::cout << "No files found.\n";
                    }
                    else
                    {
                        for (const auto& object : objects)
                        {
                            std::cout << " - "
                                      << object.GetKey()
                                      << "\n";
                        }
                    }
                }
            }

            // ---------------- DOWNLOAD ----------------
            else if (choice == 3)
            {
                std::string fileName;

                std::cout << "Enter file name to download: ";
                std::cin >> fileName;

                Aws::S3::Model::GetObjectRequest request;

                request.SetBucket(bucketName);
                request.SetKey(fileName);

                auto outcome = s3Client.GetObject(request);

                if (!outcome.IsSuccess())
                {
                    std::cout << "Download failed: "
                              << outcome.GetError().GetMessage()
                              << "\n";
                }
                else
                {
                    std::string outputName = "downloaded_" + fileName;

                    std::ofstream outputFile(
                        outputName,
                        std::ios::binary
                    );

                    outputFile << outcome.GetResult().GetBody().rdbuf();

                    outputFile.close();

                    std::cout << "File downloaded successfully!\n";
                    std::cout << "Saved as: " << outputName << "\n";

                    // Calculate hash of the downloaded file
                    std::string downloadedHash = calculateSHA256(outputName);

                    // Get original hash stored in S3 metadata
                    Aws::String originalHash =
                        outcome.GetResult().GetMetadata().at("sha256");

                    std::cout << "Original SHA-256: "
                            << originalHash << "\n";

                    std::cout << "Downloaded SHA-256: "
                            << downloadedHash << "\n";

                    // Compare both hashes
                    if (downloadedHash == originalHash)
                    {
                        std::cout << "Integrity check PASSED! File is unchanged.\n"; 
                        logEvent(logsClient, "Download successful and SHA-256 verified: " + fileName);
                    }
                    else
                    {
                        std::cout << "Integrity check FAILED! File may have changed.\n";
                    }

                }
            }

            // ---------------- DELETE ----------------
            else if (choice == 4)
            {
                std::string fileName;

                std::cout << "Enter file name to delete: ";
                std::cin >> fileName;

                Aws::S3::Model::DeleteObjectRequest request;

                request.SetBucket(bucketName);
                request.SetKey(fileName);

                auto outcome = s3Client.DeleteObject(request);

                if (!outcome.IsSuccess())
                {
                    std::cout << "Delete failed: "
                              << outcome.GetError().GetMessage()
                              << "\n";
                }
                else
                {
                    std::cout << "File deleted successfully!\n";                    
                    logEvent(logsClient, "File deleted successfully: " + fileName);
                }
            }

            // ---------------- EXIT ----------------
            else if (choice == 5)
            {
                std::cout << "Exiting CloudVault...\n";
                break;
            }

            else
            {
                std::cout << "Invalid choice.\n";
            }
        }
    }

    Aws::ShutdownAPI(options);

    return 0;
}