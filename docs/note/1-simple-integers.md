# FHE for arithmetic over integers (BFV)

- [Simple Code Example](../../src/pke/examples/simple-integers.cpp)

- [Simple Code Example with Serialization](../../src/pke/examples/simple-integers-serial.cpp)

## 运行

安装后直接调用即可

```shell
cd openfhe-development/build/bin/examples/pke
./simple-integers
```

## Simple Code Example 代码解析

首先设置了多项式的度 65537，乘法深度 2，并选择想要启用的功能：

```cpp
    // Sample Program: Step 1: Set CryptoContext
    CCParams<CryptoContextBFVRNS> parameters;
    parameters.SetPlaintextModulus(65537);
    parameters.SetMultiplicativeDepth(2);

    CryptoContext<DCRTPoly> cryptoContext = GenCryptoContext(parameters);
    // Enable features that you wish to use
    cryptoContext->Enable(PKE);
    cryptoContext->Enable(KEYSWITCH);
    cryptoContext->Enable(LEVELEDSHE);
```

接下来生成需要的各种密钥，如公私钥、重线性化密钥和旋转计算密钥：

```cpp
    // Sample Program: Step 2: Key Generation

    // Initialize Public Key Containers
    KeyPair<DCRTPoly> keyPair;

    // Generate a public/private key pair
    keyPair = cryptoContext->KeyGen();

    // Generate the relinearization key
    cryptoContext->EvalMultKeyGen(keyPair.secretKey);

    // Generate the rotation evaluation keys
    cryptoContext->EvalRotateKeyGen(keyPair.secretKey, {1, 2, -1, -2});
```

接下来是输入需要的明文，这里准备了三个长度为 12 的向量作为明文，通过 `MakePackedPlaintext()` 函数将输入向量编码为明文，再通过 `Encrypt(keyPair.publicKey, plaintext)` 函数使用公钥加密为密文：

```cpp
    // Sample Program: Step 3: Encryption

    // First plaintext vector is encoded
    std::vector<int64_t> vectorOfInts1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    Plaintext plaintext1               = cryptoContext->MakePackedPlaintext(vectorOfInts1);
    // Second plaintext vector is encoded
    std::vector<int64_t> vectorOfInts2 = {3, 2, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    Plaintext plaintext2               = cryptoContext->MakePackedPlaintext(vectorOfInts2);
    // Third plaintext vector is encoded
    std::vector<int64_t> vectorOfInts3 = {1, 2, 5, 2, 5, 6, 7, 8, 9, 10, 11, 12};
    Plaintext plaintext3               = cryptoContext->MakePackedPlaintext(vectorOfInts3);

    // The encoded vectors are encrypted
    auto ciphertext1 = cryptoContext->Encrypt(keyPair.publicKey, plaintext1);
    auto ciphertext2 = cryptoContext->Encrypt(keyPair.publicKey, plaintext2);
    auto ciphertext3 = cryptoContext->Encrypt(keyPair.publicKey, plaintext3);
```

下面进行密文的加法、乘法与旋转操作，将三个密文相加，相乘，并对密文1进行了左转 1、左转 2、右转 1、右转 2 的操作（正数是左转，负数是右转，注意这里说的左转右转是循环移位）：

```cpp
    // Sample Program: Step 4: Evaluation

    // Homomorphic additions
    auto ciphertextAdd12     = cryptoContext->EvalAdd(ciphertext1, ciphertext2);
    auto ciphertextAddResult = cryptoContext->EvalAdd(ciphertextAdd12, ciphertext3);

    // Homomorphic multiplications
    auto ciphertextMul12      = cryptoContext->EvalMult(ciphertext1, ciphertext2);
    auto ciphertextMultResult = cryptoContext->EvalMult(ciphertextMul12, ciphertext3);

    // Homomorphic rotations
    auto ciphertextRot1 = cryptoContext->EvalRotate(ciphertext1, 1);
    auto ciphertextRot2 = cryptoContext->EvalRotate(ciphertext1, 2);
    auto ciphertextRot3 = cryptoContext->EvalRotate(ciphertext1, -1);
    auto ciphertextRot4 = cryptoContext->EvalRotate(ciphertext1, -2);
```

最后就是解密和收获结果

- 通过函数 `Decrypt(keyPair.secretKey, ciphertextResult, &plaintextResult)` 使用私钥进行解密，对于向量结果再定义一下它的长度就可以了：

```cpp
    // Sample Program: Step 5: Decryption

    // Decrypt the result of additions
    Plaintext plaintextAddResult;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextAddResult, &plaintextAddResult);

    // Decrypt the result of multiplications
    Plaintext plaintextMultResult;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextMultResult, &plaintextMultResult);

    // Decrypt the result of rotations
    Plaintext plaintextRot1;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextRot1, &plaintextRot1);
    Plaintext plaintextRot2;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextRot2, &plaintextRot2);
    Plaintext plaintextRot3;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextRot3, &plaintextRot3);
    Plaintext plaintextRot4;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextRot4, &plaintextRot4);

    plaintextRot1->SetLength(vectorOfInts1.size());
    plaintextRot2->SetLength(vectorOfInts1.size());
    plaintextRot3->SetLength(vectorOfInts1.size());
    plaintextRot4->SetLength(vectorOfInts1.size());

    std::cout << "Plaintext #1: " << plaintext1 << std::endl;
    std::cout << "Plaintext #2: " << plaintext2 << std::endl;
    std::cout << "Plaintext #3: " << plaintext3 << std::endl;

    // Output results
    std::cout << "\nResults of homomorphic computations" << std::endl;
    std::cout << "#1 + #2 + #3: " << plaintextAddResult << std::endl;
    std::cout << "#1 * #2 * #3: " << plaintextMultResult << std::endl;
    std::cout << "Left rotation of #1 by 1: " << plaintextRot1 << std::endl;
    std::cout << "Left rotation of #1 by 2: " << plaintextRot2 << std::endl;
    std::cout << "Right rotation of #1 by 1: " << plaintextRot3 << std::endl;
    std::cout << "Right rotation of #1 by 2: " << plaintextRot4 << std::endl;

    return 0;
```

最后输出结果：

```shell
Plaintext #1: ( 1 2 3 4 5 6 7 8 9 10 11 12 ... )
Plaintext #2: ( 3 2 1 4 5 6 7 8 9 10 11 12 ... )
Plaintext #3: ( 1 2 5 2 5 6 7 8 9 10 11 12 ... )

Results of homomorphic computations
#1 + #2 + #3: ( 5 6 9 10 15 18 24 27 30 33 36 ... )
#1 * #2 * #3: ( 3 8 15 32 125 216 343 512 729 1000 1331 1728 ... )
Left rotation of #1 by 1: ( 2 3 4 5 6 7 8 9 10 11 12 ... )
Left rotation of #1 by 2: ( 3 4 5 6 7 8 9 10 11 12 ...)
Right rotation of #1 by 1: (0 1 2 3 4 5 6 7 8 9 10 11 ... )
Right rotation of #1 by 2: (0 0 1 2 3 4 5 6 7 8 9 10 ... )
```

## Simple Code Example with Serialization 代码解析

相比 [Simple Code Example.cpp](#simple-code-example-代码解析)，多了几个用于序列化的头文件

```cpp
// header files needed for serialization
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/bfvrns/bfvrns-ser.h"
```

## 可主动输入计算代码解析

代码参考[小雪月-OpenFHE库学习第二天](https://zhuanlan.zhihu.com/p/714671905)

运行方式：

- 首先将下面的源码放到 OpenFHE 库中的 example 文件夹，路径为：'/src/pke/examples/'

- 如果命名为 'simple-integers-interaction.cpp'

- 编译

```shell
cd OpenFHE/build
make
```

- 运行：

```shell
cd OpenFHE/build/bin/examples/pke
./simple-integers-interaction.cpp
```

源代码如下

```cpp
#include "openfhe.h"
#include <iostream>
#include <vector>
#include <stdexcept>
 
using namespace lbcrypto;
 
extern "C" {
    void perform_homomorphic_operations() {
        try {
            // 第一步：设置加密上下文
            // 创建加密上下文参数对象
            CCParams<CryptoContextBFVRNS> parameters;
            // 设置明文模数为65537
            parameters.SetPlaintextModulus(65537);
            // 设置乘法深度为2
            parameters.SetMultiplicativeDepth(2);
 
            // 生成加密上下文
            CryptoContext<DCRTPoly> cryptoContext = GenCryptoContext(parameters);
            // 启用公钥加密功能
            cryptoContext->Enable(PKE);
            // 启用密钥切换功能
            cryptoContext->Enable(KEYSWITCH);
            // 启用分层同态加密功能
            cryptoContext->Enable(LEVELEDSHE);
 
            // 第二步：生成密钥对
            // 生成密钥对对象
            KeyPair<DCRTPoly> keyPair;
            // 生成密钥对
            keyPair = cryptoContext->KeyGen();
            // 生成同态乘法所需的密钥
            cryptoContext->EvalMultKeyGen(keyPair.secretKey);
            // 生成同态旋转所需的密钥，旋转位置为1, 2, -1, -2
            cryptoContext->EvalRotateKeyGen(keyPair.secretKey, {1, 2, -1, -2});
 
            // 第三步：获取用户输入
            int x;
            std::cout << "请输入要输入的值的数量：";
            std::cin >> x;
 
            // 检查输入数量是否为正整数
            if (x <= 0) {
                throw std::invalid_argument("输入的数量必须是正整数");
            }
 
            // 创建一个向量来存储用户输入的值
            std::vector<int64_t> userInput(x);
            std::cout << "请输入 " << x << " 个值：";
            for (int i = 0; i < x; ++i) {
                std::cin >> userInput[i];
            }
 
            // 第四步：加密
            // 将用户输入的值转换为明文
            Plaintext plaintext = cryptoContext->MakePackedPlaintext(userInput);
            // 加密明文，生成密文
            auto ciphertext = cryptoContext->Encrypt(keyPair.publicKey, plaintext);
 
            // 第五步：同态运算
            // 创建一个包含顺序值 1, 2, ..., x 的向量
            std::vector<int64_t> seqValues(x);
            for (int i = 0; i < x; ++i) {
                seqValues[i] = i + 1;
            }
            // 将顺序值向量转换为明文
            Plaintext seqPlaintext = cryptoContext->MakePackedPlaintext(seqValues);
            // 加密顺序值明文，生成密文
            auto seqCiphertext = cryptoContext->Encrypt(keyPair.publicKey, seqPlaintext);
 
            // 同态加法，将userInput与seqValues相加，循环执行x次，结果为userInput + x * seqValues
            auto ciphertextAdd = ciphertext;
            for (int i = 0; i < x; ++i) {
                ciphertextAdd = cryptoContext->EvalAdd(ciphertextAdd, seqCiphertext);
            }
 
            // 同态乘法，将userInput与seqValues相乘，循环执行x次，结果为userInput * (seqValues ^ x)
            auto ciphertextMult = ciphertext;
            for (int i = 0; i < x; ++i) {
                ciphertextMult = cryptoContext->EvalMult(ciphertextMult, seqCiphertext);
            }
 
            // 第六步：解密
            // 解密同态加法结果
            Plaintext plaintextAddResult, plaintextMultResult;
            cryptoContext->Decrypt(keyPair.secretKey, ciphertextAdd, &plaintextAddResult);
            // 解密同态乘法结果
            cryptoContext->Decrypt(keyPair.secretKey, ciphertextMult, &plaintextMultResult);
 
            // 输出解密结果
            std::cout << "同态加法结果：";
            for (int i = 0; i < x; ++i) {
                std::cout << plaintextAddResult->GetPackedValue()[i] << " ";
            }
            std::cout << std::endl;
 
            std::cout << "同态乘法结果：";
            for (int i = 0; i < x; ++i) {
                std::cout << plaintextMultResult->GetPackedValue()[i] << " ";
            }
            std::cout << std::endl;
 
        } catch (const std::exception &e) {
            // 捕获并打印异常信息
            std::cerr << "发生错误：" << e.what() << std::endl;
        }
    }
}
 
// 添加 main 函数作为程序入口点
int main() {
    perform_homomorphic_operations();
    return 0;
}
```

运行结果如下：

输入两个值的时候，向量 userInput 由终端输入为 5，6，创建的 seqValues 向量为 1，2，因此同态加法为 $5 + 1 + 1 = 7，6 + 2 + 2 = 10$，同态乘法为 $5 \times (1^2) = 5, 6 \times (2^2) = 24$。
