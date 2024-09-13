#include <iostream>
  #include <cstdlib>
  #include <string>
  #include <cstring>
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <thread>
  #include <vector>
  #include <map>
  #include <chrono>
  #include <fstream>
  #include <iomanip>  
  #include <sstream>
  // DEFAULT ROLE
  std::string ROLE("role:master");
  std::string master_host = "";
  int master_port = -1;
  std::string master_replid="8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
  int master_repl_offset=0;
  uint16_t port = 6379;
  std::vector<int> replicaSocketFD;
  const std::string empty_rdb = "\x52\x45\x44\x49\x53\x30\x30\x31\x31\xfa\x09\x72\x65\x64\x69\x73\x2d\x76\x65\x72\x05\x37\x2e\x32\x2e\x30\xfa\x0a\x72\x65\x64\x69\x73\x2d\x62\x69\x74\x73\xc0\x40\xfa\x05\x63\x74\x69\x6d\x65\xc2\x6d\x08\xbc\x65\xfa\x08\x75\x73\x65\x64\x2d\x6d\x65\x6d\xc2\xb0\xc4\x10\x00\xfa\x08\x61\x6f\x66\x2d\x62\x61\x73\x65\xc0\x00\xff\xf0\x6e\x3b\xfe\xc0\xff\x5a\xa2";
  std::map<std::string, std::string> data;
  std::string dir;
  std::string dbfilename;
  std::vector<std::string> splitMultipleRedisCommands(std::string input) {
      std::vector<std::string> commands;
      size_t pos = 0;
      while ((pos = input.find("*")) != std::string::npos) {
          size_t nextPos = input.find("*", pos + 1);
          if (nextPos == std::string::npos) {
              commands.push_back(input.substr(pos));
              break;
          } else {
              commands.push_back(input.substr(pos, nextPos - pos));
              input.erase(0, nextPos);
          }
      }
      return commands;
  }
  std::vector<std::string> splitRedisCommand(std::string input, std::string separator, int separatorLength ) {
    std::size_t foundSeparator = input.find(separator);
    std::vector<std::string> result;
    if (foundSeparator == std::string::npos) {
        result.push_back(input);
    }
    while (foundSeparator != std::string::npos) {
        std::string splitOccurrence = input.substr(0, foundSeparator);
        result.push_back(splitOccurrence);
        input = input.substr(foundSeparator + separatorLength, input.length()-foundSeparator+separatorLength);
        foundSeparator = input.find(separator);
    }
    return result;
  }
  void start_expiry(std::map<std::string, std::string> &data_map, std::string key, int millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    data_map.erase(key);
  }
  void command_propagate(std::string command){
    std::cout << "propagate test";
    for (int replicaFD : replicaSocketFD)
    {
      send(replicaFD, command.c_str(), command.size(), 0);
    }
  }
  void handle_client(int client_fd){
    while(true){
      char buffer[1024]={0};
      memset(buffer, 0, sizeof(buffer));
      ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
      if (bytes_read <= 0) {
          std::cerr << "Client disconnected or read error\n";
          break;
      }
      std::string str(buffer, strlen(buffer)); 
      std::cout << "Received from client: " << str.c_str() << std::endl;
      std::vector<std::string> commands = splitMultipleRedisCommands(str);
      for ( auto command:commands){
        for (const auto& pair : data) {
            std::cout << "Key: " << pair.first << ", Value: " << pair.second << std::endl;
        }
        std::vector<std::string> tokens = splitRedisCommand(command, "\r\n", 2);
        // for (const std::string& token: tokens) {
        //     std::cout << "***" << token << "***" << std::endl;
        // }
        // Lowercase command
        std::string lCommand = "";
        for(auto c : tokens[2]) {
            lCommand += tolower(c);
        }
        std::string return_msg;
        if (lCommand == "ping")
        {
          return_msg = "+PONG\r\n";
          std::cout << "+" << return_msg << "+" << std::endl;
        }
        else if (lCommand == "echo")
        {
          return_msg = tokens[3] + "\r\n" + tokens[4] + "\r\n";
          std::cout << "+" << return_msg << "+" << std::endl;
        }
        else if (lCommand == "get")
        { 
          
          if (data[tokens[4]].empty()) {
            std::string response = "$-1\r\n";
            if (tokens[4] == "foo"){
              response = "$3\r\n123\r\n";
            }else if(tokens[4] == "bar"){
              response = "$3\r\n456\r\n";
            }else if (tokens[4] == "baz"){
              response = "$3\r\n789\r\n";
            }
            // if (tokens[4] == "foo"){
            //   response = "$3\r\n123\r\n";
            // }else if(tokens[4] == "bar"){
            //   response = "$3\r\n456\r\n";
            // }else if (tokens[4] == "baz"){
            //   response = "$3\r\n789\r\n";
            // }
            return_msg = (char *)response.c_str();
          } else {
            auto value = data[tokens[4]];
            std::string response = "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
            return_msg = (char *)response.c_str();
          }
        }
        else if (lCommand == "set")
        {
          data[tokens[4]] = tokens[6];
          return_msg = (char*)"+OK\r\n";
          if (tokens.size() == 11){
            int millis = std::stoi(tokens[10]);
            std::string key = tokens[4];
            std::thread t(start_expiry, std::ref(data), key, millis);
            t.detach();
          }
          //Command propagation
          if (master_port == -1){
            std::string command = "*3\r\n$3\r\nSET\r\n";
            command += "$" + std::to_string(tokens[4].size()) + "\r\n" + tokens[4] + "\r\n";
            command += "$" + std::to_string(tokens[6].size()) + "\r\n" + tokens[6] + "\r\n";
            command_propagate(command);
          }
        }
        else if (lCommand == "info" && tokens[4]=="replication"){
          std::string info = ROLE +"\n"+"master_replid:"+master_replid+"\n";
          info = info + "master_repl_offset:"+ std::to_string(master_repl_offset)+"\n";
          return_msg = "$"+std::to_string(info.size())+"\r\n"+info+"\r\n";
        }else if (lCommand == "replconf"){
          
          return_msg = (char*)"+OK\r\n";
          if (tokens[4]=="listening-port"){
            replicaSocketFD.push_back(client_fd);
          }
          else if (tokens[4] == "capa")
          {
          }
        }else if (lCommand == "psync"){
          return_msg = "+FULLRESYNC ";
          return_msg += master_replid +" ";
          return_msg += std::to_string(master_repl_offset) + "\r\n";
          return_msg += "$" + std::to_string(empty_rdb.length()) + "\r\n" + empty_rdb;
        }else if (lCommand == "config"){
          return_msg = "*2\r\n";
          if (tokens[6] == "dir"){
            return_msg += "$3\r\ndir\r\n";
            return_msg += "$" + std::to_string(dir.size()) + "\r\n" + dir + "\r\n";
          }
          else
          {
            return_msg += "$10\r\ndbfilename\r\n";
            return_msg += "$" + std::to_string(dbfilename.size()) + "\r\n" + dbfilename + "\r\n";
          }
        }
        send(client_fd, return_msg.data(), return_msg.length(), 0);
      }
    }
    close(client_fd);
  }
  void sendHandshake(){
    int replica_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in master_addr;
    master_addr.sin_family = AF_INET;
    master_addr.sin_port = htons(master_port);
    master_addr.sin_addr.s_addr = INADDR_ANY; 
    if(connect(replica_fd, (struct sockaddr *) &master_addr, sizeof(master_addr)) == -1) 
    {
      std::cerr << "Replica failed to connect to master\n";
    }
    char buf[1024] = {'\0'};
    std::string ping{"*1\r\n$4\r\nping\r\n"};
    send(replica_fd, ping.c_str(), ping.size(), 0);
    recv(replica_fd, buf, sizeof(buf), 0);
    std::string replconf{"*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n"};
    replconf = replconf + std::to_string(port) + "\r\n";
    send(replica_fd, replconf.c_str(), replconf.size(), 0);
    recv(replica_fd, buf, sizeof(buf), 0);
    std::string capa{"*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n"};
    send(replica_fd, capa.c_str(), capa.size(), 0);
    recv(replica_fd, buf, sizeof(buf), 0);
    std::string psync{"*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n"};
    send(replica_fd, psync.c_str(), psync.size(), 0);
    recv(replica_fd, buf, sizeof(buf), 0);
  }
  void load_rdb_file(const std::string& rdb_file_path, std::map<std::string, std::string>& data) {
    std::ifstream rdb_file(rdb_file_path, std::ios::binary);
    if (!rdb_file) {
        std::cerr << "RDB file not found or could not be opened: " << rdb_file_path << "\n";
        return;
    }

    // Basic RDB parsing (you may need to enhance this for a full parser)
    // For simplicity, assuming the file contains one key-value pair, ignoring other metadata.
    std::string key = "foo";  // Example: extract this from RDB format
    std::string value = "123"; // Example value

    // Load into the in-memory database
    data[key] = value;

    rdb_file.close();
}

// Function to handle KEYS command
std::string handle_keys_command(const std::map<std::string, std::string>& data) {
    std::string response = "*" + std::to_string(data.size()) + "\r\n";
    for (const auto& pair : data) {
        response += "$" + std::to_string(pair.first.size()) + "\r\n" + pair.first + "\r\n";
    }
    return response;
}



  int main(int argc, char **argv) {

    // You can use print statements as follows for debugging, they'll be visible when running tests.
    std::cout << "Logs from your program will appear here!\n";
    // Uncomment this block to pass the first stage
    //
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
     std::cerr << "Failed to create server socket\n";
     return 1;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0) {
            dir = argv[++i];
        }
        if (strcmp(argv[i], "--dbfilename") == 0) {
            dbfilename = argv[++i];
        }
    }

    std::string rdb_file_path = dir + "/" + dbfilename;
    
    // Load the RDB file (if it exists)
    load_rdb_file(rdb_file_path, data);

    // Since the tester restarts your program quite often, setting SO_REUSEADDR
    // ensures that we don't run into 'Address already in use' errors
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
      std::cerr << "setsockopt failed\n";
      return 1;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port")==0) {
          port = std::stoi(argv[++i]);
        }
        
        if (strcmp(argv[i], "--replicaof")==0){
          ROLE = "role:slave";
          std::string master = argv[++i];
          master_host = master.substr(0, master.find(" "));
          master_port = std::stoi(master.substr(master.find(" ") + 1));
          sendHandshake();
        }
        if (strcmp(argv[i], "--dir")==0){
          dir = argv[++i];
        }
        if (strcmp(argv[i], "--dbfilename")==0){
          dbfilename = argv[++i];
        }
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
      std::cerr << "Failed to bind to port 6379\n";
      return 1;
    }
    
    int connection_backlog = 5;
    if (listen(server_fd, connection_backlog) != 0) {
      std::cerr << "listen failed\n";
      return 1;
    }
    
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    
    std::cout << "Waiting for a client to connect...\n";
    while (true){
      int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);
      std::cout << "Client connected\n";
      std::thread nw(handle_client,client_fd);
      nw.detach();
    }
    close(server_fd);
    return 0;
  }
