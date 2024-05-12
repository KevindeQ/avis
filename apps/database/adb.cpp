#include <iostream>

#include "avis/core/common.h"
#include "asio.hpp"

using asio::ip::tcp;

std::string make_daytime_string()
{
    using namespace std; // For time_t, time and ctime;
    time_t now = time(0);
    return ctime(&now);
}

class tcp_connection : public std::enable_shared_from_this<tcp_connection>
{
public:
    typedef std::shared_ptr<tcp_connection> pointer;

    static pointer create(asio::io_context& io_context)
    {
        return pointer{ new tcp_connection(io_context) };
    }

    tcp::socket& socket()
    {
        return socket_;
    }

    void start()
    {
        message_ = make_daytime_string();

        asio::async_write(
            socket_,
            asio::buffer(message_),
            std::bind(
                &tcp_connection::handle_write,
                shared_from_this(),
                asio::placeholders::error,
                asio::placeholders::bytes_transferred));
    }

private:
    tcp_connection(asio::io_context& io_context) : socket_(io_context) {}

    void handle_write(const asio::error_code& /*error*/, size_t /*bytes_transferred*/) {}

    tcp::socket socket_;
    std::string message_;
};

class tcp_server
{
public:
    tcp_server(asio::io_context& io_context) :
        io_context_{ io_context },
        acceptor_{ io_context, tcp::endpoint(tcp::v4(), 13) }
    {
        start_accept();
    }

private:
    void start_accept()
    {
        tcp_connection::pointer new_connection = tcp_connection::create(io_context_);

        acceptor_.async_accept(
            new_connection->socket(),
            std::bind(&tcp_server::handle_accept, this, new_connection, asio::placeholders::error));
    }

    void handle_accept(tcp_connection::pointer new_connection, const asio::error_code& error)
    {
        if (!error)
        {
            new_connection->start();
        }

        start_accept();
    }

    asio::io_context& io_context_;
    tcp::acceptor acceptor_;
};

int main(int argc, char* argv[])
{
    try
    {
        asio::io_context io_context;
        tcp_server server{ io_context };
        io_context.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }

    return 0;

    //std::filesystem::path source_directory{
    //    "E:\\Data\\KITTI-360\\data_3d_semantics\\train\\2013_05_28_drive_0000_sync\\static\\"
    //};
    //std::vector<std::future<geometry::data_store>> geometry_data{};

    //for (const std::filesystem::directory_entry& entry :
    //     std::filesystem::recursive_directory_iterator(source_directory))
    //{
    //    if (!entry.is_regular_file())
    //    {
    //        continue;
    //    }

    //    std::filesystem::path p = entry.path();
    //    if (p.extension() != ".ply")
    //    {
    //        continue;
    //    }

    //    io::file_descriptor ply_file = file_context.create_descriptor(p);
    //    /*std::future<geometry::data_store> file_contents = file_load_service.async_read_file<geometry::data_store>(
    //        ply_file,
    //        [](const streams::memory_stream& data)
    //        {
    //            data_formats::ply::ply_parser parser;
    //            return parser.parse(data);
    //        });
    //    geometry_data.push_back(file_contents);*/
    //}

    //io::basic_file_descriptor database_file{ "E:\\Projects\\avis\\assets\\maps\\point_cloud.adf" };
    //io::basic_file_descriptor index_file{ "E:\\Projects\\avis\\assets\\maps\\point_cloud.aif" };
    //data::point_cloud database{ index_file, database_file };

    //for (std::future<geometry::data_store>& data_store : geometry_data)
    //{
    //    geometry::data_store ds = data_store.get();
    //}

    return 0;
}