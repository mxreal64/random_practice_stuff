// Copyright (C) 2026 mxreal64
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://gnu.org>.

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
