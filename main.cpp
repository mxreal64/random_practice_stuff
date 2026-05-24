import std;
import CompileTimeDI;

using namespace ctdi;

struct Database {
    std::string connection_string = "Ultra_Optimized_C++26_DB";
};

struct UserRepository {
    Database& db; 
};

struct UserService {
    UserRepository repo; 
};

int main() {
    constexpr CompileTimeDI<
        ServiceDescriptor<Database, Lifetime::Singleton>,
        ServiceDescriptor<UserRepository, Lifetime::Transient>,
        ServiceDescriptor<UserService, Lifetime::Transient>
    > container;

    auto userService = container.resolve<UserService>();
    std::println("DB Target: {}", userService.repo.db.connection_string);

    return 0;
}
