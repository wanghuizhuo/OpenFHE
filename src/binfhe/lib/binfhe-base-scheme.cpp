//==================================================================================
// BSD 2-Clause License
//
// Copyright (c) 2014-2022, NJIT, Duality Technologies Inc. and other contributors
//
// All rights reserved.
//
// Author TPOC: contact@openfhe.org
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//==================================================================================

#include "binfhe-base-scheme.h"

#include <string>

namespace lbcrypto {

// wrapper for KeyGen methods
RingGSWBTKey BinFHEScheme::KeyGen(const std::shared_ptr<BinFHECryptoParams>& params, ConstLWEPrivateKey& LWEsk,
                                  KEYGEN_MODE keygenMode = SYM_ENCRYPT) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (LWEsk == nullptr)
        OPENFHE_THROW("PrivateKey is empty");

    const auto& LWEParams = params->GetLWEParams();

    RingGSWBTKey ek;
    LWEPrivateKey skN;
    if (keygenMode == SYM_ENCRYPT) {
        skN = LWEscheme->KeyGen(LWEParams->GetN(), LWEParams->GetQ());
    }
    else if (keygenMode == PUB_ENCRYPT) {
        ConstLWEKeyPair kpN = LWEscheme->KeyGenPair(LWEParams);
        skN                 = kpN->secretKey;
        ek.Pkey             = kpN->publicKey;
    }
    else {
        OPENFHE_THROW("Invalid KeyGen mode");
    }

    ek.KSkey = LWEscheme->KeySwitchGen(LWEParams, LWEsk, skN);

    const auto& RGSWParams = params->GetRingGSWParams();
    const auto& polyParams = RGSWParams->GetPolyParams();
    NativePoly skNPoly(polyParams);
    skNPoly.SetValues(std::move(skN->GetElement()), Format::COEFFICIENT);
    skNPoly.SetFormat(Format::EVALUATION);

    ek.BSkey = ACCscheme->KeyGenAcc(RGSWParams, skNPoly, LWEsk);

    return ek;
}

// Full evaluation as described in https://eprint.iacr.org/2020/086
LWECiphertext BinFHEScheme::EvalBinGate(const std::shared_ptr<BinFHECryptoParams>& params, BINGATE gate,
                                        const RingGSWBTKey& EK, ConstLWECiphertext& ct1,
                                        ConstLWECiphertext& ct2, bool extended) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct1 == nullptr)
        OPENFHE_THROW("Ciphertext1 is empty");
    if (ct2 == nullptr)
        OPENFHE_THROW("Ciphertext2 is empty");
    if (ct1 == ct2)
        OPENFHE_THROW("Input ciphertexts should be independant");

    const auto& LWEParams = params->GetLWEParams();
    NativeInteger Q{LWEParams->GetQ()};

    // input cts expected with SMALL_DIM
    auto cct1       = (Q == ct1->GetModulus()) ? LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ct1) : std::make_shared<LWECiphertextImpl>(*ct1);
    const auto cct2 = (Q == ct2->GetModulus()) ? LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ct2) : ct2;

    // the additive homomorphic operation for XOR/NXOR is different from the other gates we compute
    // 2*(ct1 + ct2) mod 4 for XOR, 0 -> 0, 2 -> 1
    // XOR_FAST and XNOR_FAST are included for backwards compatibility; they map to XOR and XNOR
    if ((gate == XOR) || (gate == XNOR) || (gate == XOR_FAST) || (gate == XNOR_FAST)) {
        LWEscheme->EvalAddEq(cct1, cct2);
        LWEscheme->EvalAddEq(cct1, cct1);
    }
    else {
        // for all other gates, we simply compute (ct1 + ct2) mod 4
        // for AND: 0,1 -> 0 and 2,3 -> 1
        // for OR: 1,2 -> 1 and 3,0 -> 0
        LWEscheme->EvalAddEq(cct1, cct2);
    }

    // the accumulator result is encrypted w.r.t. the transposed secret key
    // we can transpose "a" to get an encryption under the original secret key
    auto accVec{BootstrapGateCore(params, gate, EK.BSkey, cct1)->GetElements()};
    accVec[0] = accVec[0].Transpose();
    accVec[0].SetFormat(Format::COEFFICIENT);
    accVec[1].SetFormat(Format::COEFFICIENT);

    // hardcoded for p = 4
    // we add Q/8 to "b" to map back to Q/4 (i.e., mod 2) arithmetic.
    NativeInteger b{(Q >> 3) + 1};
    b.ModAddFastEq(accVec[1][0], Q);

    auto ctExt = std::make_shared<LWECiphertextImpl>(std::move(accVec[0].GetValues()), b);

    if (extended)
        return ctExt;
    return LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ctExt);
}

// Full evaluation as described in https://eprint.iacr.org/2020/086
LWECiphertext BinFHEScheme::EvalBinGate(const std::shared_ptr<BinFHECryptoParams>& params, BINGATE gate,
                                        const RingGSWBTKey& EK, const std::vector<LWECiphertext>& ctvector, bool extended) const {

    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");

    // check if the ciphertexts are all independent
    uint32_t length = ctvector.size();
    for (uint32_t i = 0; i < length; ++i) {
        for (uint32_t j = i + 1; j < length; ++j) {
            if (ctvector[i] == ctvector[j]) {
                OPENFHE_THROW("Input ciphertexts should be independent");
            }
        }
    }

    if ((gate == AND3) || (gate == OR3) || (gate == AND4) || (gate == OR4) || (gate == MAJORITY)) {
        const auto& LWEParams = params->GetLWEParams();
        NativeInteger Q{LWEParams->GetQ()};

        // input cts expected with SMALL_DIM
        auto ct = (Q == ctvector[0]->GetModulus()) ? LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ctvector[0]) : std::make_shared<LWECiphertextImpl>(*ctvector[0]);
        for (uint32_t i = 1; i < length; ++i) {
            LWEscheme->EvalAddEq(ct, (Q == ctvector[i]->GetModulus()) ? LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ctvector[i]) : ctvector[i]);
        }

        auto p = ctvector[0]->GetptModulus();
        ct->SetptModulus(p);

        // the accumulator result is encrypted w.r.t. the transposed secret key
        // we can transpose "a" to get an encryption under the original secret key
        auto accVec{BootstrapGateCore(params, gate, EK.BSkey, ct)->GetElements()};
        accVec[0] = accVec[0].Transpose();
        accVec[0].SetFormat(Format::COEFFICIENT);
        accVec[1].SetFormat(Format::COEFFICIENT);

        NativeInteger b = Q / (p * 2) + 1;
        b.ModAddFastEq(accVec[1][0], Q);

        auto ctExt = std::make_shared<LWECiphertextImpl>(std::move(accVec[0].GetValues()), b);

        if (!extended)
            ctExt = LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ctExt);

        ctExt->SetptModulus(p);
        return ctExt;
    }
    else if (gate == CMUX) {
        if (length != 3)
            OPENFHE_THROW("CMUX gate implemented for ciphertext vectors of size 3");

        auto&& ctNAND1 = EvalBinGate(params, NAND, EK, ctvector[0], EvalNOT(params, ctvector[2]));
        auto&& ctNAND2 = EvalBinGate(params, NAND, EK, ctvector[1], ctvector[2]);
        return EvalBinGate(params, NAND, EK, ctNAND1, ctNAND2);
    }
    else {
        OPENFHE_THROW("This gate is not implemented for vector of ciphertexts at this time");
    }
}

// Full evaluation as described in https://eprint.iacr.org/2020/086
LWECiphertext BinFHEScheme::Bootstrap(const std::shared_ptr<BinFHECryptoParams>& params, const RingGSWBTKey& EK,
                                      ConstLWECiphertext& ct, bool extended) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");

    const auto& LWEParams = params->GetLWEParams();
    NativeInteger Q{LWEParams->GetQ()};
    // input ct expected with SMALL_DIM
    auto cct = (Q == ct->GetModulus()) ? LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ct) : std::make_shared<LWECiphertextImpl>(*ct);
    LWEscheme->EvalAddConstEq(cct, (ct->GetModulus() >> 2));

    // the accumulator result is encrypted w.r.t. the transposed secret key
    // we can transpose "a" to get an encryption under the original secret key
    auto accVec{BootstrapGateCore(params, AND, EK.BSkey, cct)->GetElements()};
    accVec[0] = accVec[0].Transpose();
    accVec[0].SetFormat(Format::COEFFICIENT);
    accVec[1].SetFormat(Format::COEFFICIENT);

    NativeInteger b{Q / (ct->GetptModulus() * 2) + 1};
    b.ModAddFastEq(accVec[1][0], Q);

    auto ctExt = std::make_shared<LWECiphertextImpl>(std::move(accVec[0].GetValues()), b);

    if (!extended)
        ctExt = LWEscheme->SwitchCTtoqn(LWEParams, EK.KSkey, ctExt);

    ctExt->SetptModulus(ct->GetptModulus());
    return ctExt;
}

// Evaluation of the NOT operation; no key material is needed
LWECiphertext BinFHEScheme::EvalNOT(const std::shared_ptr<BinFHECryptoParams>& params, ConstLWECiphertext& ct) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");

    NativeInteger q{ct->GetModulus()};
    uint32_t n{ct->GetLength()};

    NativeVector a(n, q);
    for (uint32_t i = 0; i < n; ++i)
        a[i] = ct->GetA(i) == 0 ? 0 : q - ct->GetA(i);

    return std::make_shared<LWECiphertextImpl>(std::move(a), (q >> 2).ModSubFast(ct->GetB(), q));
}

// Evaluate Arbitrary Function homomorphically
// Modulus of ct is q | 2N
LWECiphertext BinFHEScheme::EvalFunc(const std::shared_ptr<BinFHECryptoParams>& params, const RingGSWBTKey& EK,
                                     ConstLWECiphertext& ct, const std::vector<NativeInteger>& LUT,
                                     const NativeInteger& beta) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");

    auto ct1 = std::make_shared<LWECiphertextImpl>(*ct);
    NativeInteger q{ct->GetModulus()};
    uint32_t functionProperty{this->checkInputFunction(LUT, q)};

    if (functionProperty == 0) {  // negacyclic function only needs one bootstrap
        auto fLUT = [LUT](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
            return LUT[x.ConvertToInt()];
        };
        LWEscheme->EvalAddConstEq(ct1, beta);
        return BootstrapFunc(params, EK, ct1, fLUT, q);
    }

    if (functionProperty == 2) {  // arbitary funciton
        const auto& LWEParams = params->GetLWEParams();
        uint32_t N{LWEParams->GetN()};
        if (q.ConvertToInt() > N) {  // need q to be at most = N for arbitary function
            std::string errMsg =
                "ERROR: ciphertext modulus q needs to be <= ring dimension for arbitrary function evaluation";
            OPENFHE_THROW(errMsg);
        }

        // TODO: figure out a way to not do this :(

        // repeat the LUT to make it periodic
        std::vector<NativeInteger> LUT2;
        LUT2.reserve(LUT.size() + LUT.size());
        LUT2.insert(LUT2.end(), LUT.begin(), LUT.end());
        LUT2.insert(LUT2.end(), LUT.begin(), LUT.end());

        NativeInteger dq{q << 1};
        // raise the modulus of ct1 : q -> 2q
        ct1->GetA().SetModulus(dq);
        auto ct2 = std::make_shared<LWECiphertextImpl>(*ct1);
        LWEscheme->EvalAddConstEq(ct2, beta);

        // this is 1/4q_small or -1/4q_small mod q
        auto f0 = [](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
            if (x < (q >> 1))
                return Q - (q >> 2);
            else
                return (q >> 2);
        };
        auto ct3 = BootstrapFunc(params, EK, ct2, f0, dq);
        LWEscheme->EvalSubEq2(ct1, ct3);
        LWEscheme->EvalAddConstEq(ct3, beta);
        LWEscheme->EvalSubConstEq(ct3, q >> 1);

        // Now the input is within the range [0, q/2).
        // Note that for non-periodic function, the input q is boosted up to 2q
        auto fLUT2 = [LUT2](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
            if (x < (q >> 1))
                return LUT2[x.ConvertToInt()];
            else
                return Q - LUT2[x.ConvertToInt() - q.ConvertToInt() / 2];
        };
        auto ct4 = BootstrapFunc(params, EK, ct3, fLUT2, dq);
        ct4->SetModulus(q);
        return ct4;
    }

    // Else it's periodic function so we evaluate directly
    LWEscheme->EvalAddConstEq(ct1, beta);
    // this is 1/4q_small or -1/4q_small mod q
    auto f0 = [](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
        if (x < (q >> 1))
            return Q - (q >> 2);
        else
            return (q >> 2);
    };
    auto ct2 = BootstrapFunc(params, EK, ct1, f0, q);
    LWEscheme->EvalSubEq2(ct, ct2);
    LWEscheme->EvalAddConstEq(ct2, beta);
    LWEscheme->EvalSubConstEq(ct2, q >> 2);

    // Now the input is within the range [0, q/2).
    // Note that for non-periodic function, the input q is boosted up to 2q
    auto fLUT1 = [LUT](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
        if (x < (q >> 1))
            return LUT[x.ConvertToInt()];
        else
            return Q - LUT[x.ConvertToInt() - q.ConvertToInt() / 2];
    };
    return BootstrapFunc(params, EK, ct2, fLUT1, q);
}

// Evaluate Homomorphic Flooring
LWECiphertext BinFHEScheme::EvalFloor(const std::shared_ptr<BinFHECryptoParams>& params, const RingGSWBTKey& EK,
                                      ConstLWECiphertext& ct, const NativeInteger& beta, uint32_t roundbits) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");

   const auto& LWEParams = params->GetLWEParams();
    NativeInteger q{roundbits == 0 ? LWEParams->Getq() : beta * (1 << (roundbits + 1))};
    NativeInteger mod{ct->GetModulus()};

    auto ct1 = std::make_shared<LWECiphertextImpl>(*ct);
    LWEscheme->EvalAddConstEq(ct1, beta);

    auto ct1Modq = std::make_shared<LWECiphertextImpl>(*ct1);
    ct1Modq->SetModulus(q);
    // this is 1/4q_small or -1/4q_small mod q
    auto f1 = [](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
        if (x < (q >> 1))
            return Q - (q >> 2);
        else
            return (q >> 2);
    };
    auto ct2 = BootstrapFunc(params, EK, ct1Modq, f1, mod);
    LWEscheme->EvalSubEq(ct1, ct2);

    auto ct2Modq = std::make_shared<LWECiphertextImpl>(*ct1);
    ct2Modq->SetModulus(q);

    // now the input is only within the range [0, q/2)
    auto f2 = [](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
        if (x < (q >> 2))
            return Q - (q >> 1) - x;
        else if (((q >> 2) <= x) && (x < 3 * (q >> 2)))
            return x;
        else
            return Q + (q >> 1) - x;
    };
    auto ct3 = BootstrapFunc(params, EK, ct2Modq, f2, mod);
    LWEscheme->EvalSubEq(ct1, ct3);

    return ct1;
}

// Evaluate large-precision sign
LWECiphertext BinFHEScheme::EvalSign(const std::shared_ptr<BinFHECryptoParams>& params,
                                     const std::map<uint32_t, RingGSWBTKey>& EKs, ConstLWECiphertext& ct,
                                     const NativeInteger& beta, bool schemeSwitch) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");

    auto mod{ct->GetModulus()};
    const auto& LWEParams = params->GetLWEParams();
    auto q{LWEParams->Getq()};
    if (mod <= q) {
        std::string errMsg =
            "ERROR: EvalSign is only for large precision. For small precision, please use bootstrapping directly";
        OPENFHE_THROW(errMsg);
    }

    const auto& RGSWParams = params->GetRingGSWParams();
    const auto curBase     = RGSWParams->GetBaseG();
    auto search            = EKs.find(curBase);
    if (search == EKs.end()) {
        std::string errMsg("ERROR: No key [" + std::to_string(curBase) + "] found in the map");
        OPENFHE_THROW(errMsg);
    }
    RingGSWBTKey curEK(search->second);

    auto cttmp = std::make_shared<LWECiphertextImpl>(*ct);
    while (mod > q) {
        cttmp = EvalFloor(params, curEK, cttmp, beta);
        // round Q to 2betaQ/q
        //  mod   = mod / q * 2 * beta;
        mod   = (mod << 1) * beta / q;
        cttmp = LWEscheme->ModSwitch(mod, cttmp);

        // if dynamic
        if (EKs.size() == 3) {
            // TODO: use GetMSB()?
            uint32_t binLog = static_cast<uint32_t>(ceil(GetMSB(mod.ConvertToInt()) - 1));
            uint32_t base{0};
            if (binLog <= static_cast<uint32_t>(17))
                base = static_cast<uint32_t>(1) << 27;
            else if (binLog <= static_cast<uint32_t>(26))
                base = static_cast<uint32_t>(1) << 18;

            if (0 != base) {  // if base is to change ...
                RGSWParams->Change_BaseG(base);

                auto search = EKs.find(base);
                if (search == EKs.end()) {
                    std::string errMsg("ERROR: No key [" + std::to_string(curBase) + "] found in the map");
                    OPENFHE_THROW(errMsg);
                }
                curEK = search->second;
            }
        }
    }
    LWEscheme->EvalAddConstEq(cttmp, beta);

    if (!schemeSwitch) {
        // if the ended q is smaller than q, we need to change the param for the final boostrapping
        auto f3 = [](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
            return (x < q / 2) ? (Q / 4) : (Q - Q / 4);
        };
        cttmp = BootstrapFunc(params, curEK, cttmp, f3, q);  // this is 1/4q_small or -1/4q_small mod q
        LWEscheme->EvalSubConstEq(cttmp, q >> 2);
    }
    else {  // return the negated f3 and do not subtract q/4 for a more natural encoding in scheme switching
        // if the ended q is smaller than q, we need to change the param for the final boostrapping
        auto f3 = [](NativeInteger x, NativeInteger q, NativeInteger Q) -> NativeInteger {
            return (x < q / 2) ? (Q - Q / 4) : (Q / 4);
        };
        cttmp = BootstrapFunc(params, curEK, cttmp, f3, q);  // this is 1/4q_small or -1/4q_small mod q
    }
    RGSWParams->Change_BaseG(curBase);
    return cttmp;
}

// Evaluate Ciphertext Decomposition
std::vector<LWECiphertext> BinFHEScheme::EvalDecomp(const std::shared_ptr<BinFHECryptoParams>& params,
                                                    const std::map<uint32_t, RingGSWBTKey>& EKs, ConstLWECiphertext& ct,
                                                    const NativeInteger& beta) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");

    auto mod         = ct->GetModulus();
    auto& LWEParams  = params->GetLWEParams();
    auto& RGSWParams = params->GetRingGSWParams();

    NativeInteger q = LWEParams->Getq();
    if (mod <= q) {
        std::string errMsg =
            "ERROR: EvalDecomp is only for large precision. For small precision, please use bootstrapping directly";
        OPENFHE_THROW(errMsg);
    }

    const auto curBase = RGSWParams->GetBaseG();
    auto search        = EKs.find(curBase);
    if (search == EKs.end()) {
        std::string errMsg("ERROR: No key [" + std::to_string(curBase) + "] found in the map");
        OPENFHE_THROW(errMsg);
    }
    RingGSWBTKey curEK(search->second);

    auto cttmp = std::make_shared<LWECiphertextImpl>(*ct);
    std::vector<LWECiphertext> ret;
    while (mod > q) {
        auto ctq = std::make_shared<LWECiphertextImpl>(*cttmp);
        ctq->SetModulus(q);
        ret.push_back(std::move(ctq));

        // Floor the input sequentially to obtain the most significant bit
        cttmp = EvalFloor(params, curEK, cttmp, beta);
        mod   = mod / q * 2 * beta;
        // round Q to 2betaQ/q
        cttmp = LWEscheme->ModSwitch(mod, cttmp);

        if (EKs.size() == 3) {  // if dynamic
            uint32_t binLog = static_cast<uint32_t>(ceil(log2(mod.ConvertToInt())));
            uint32_t base   = 0;
            if (binLog <= static_cast<uint32_t>(17))
                base = static_cast<uint32_t>(1) << 27;
            else if (binLog <= static_cast<uint32_t>(26))
                base = static_cast<uint32_t>(1) << 18;

            if (0 != base) {  // if base is to change ...
                RGSWParams->Change_BaseG(base);

                auto search = EKs.find(base);
                if (search == EKs.end()) {
                    std::string errMsg("ERROR: No key [" + std::to_string(curBase) + "] found in the map");
                    OPENFHE_THROW(errMsg);
                }
                curEK = search->second;
            }
        }
    }
    RGSWParams->Change_BaseG(curBase);
    ret.push_back(std::move(cttmp));
    return ret;
}

// private:

RLWECiphertext BinFHEScheme::BootstrapGateCore(const std::shared_ptr<BinFHECryptoParams>& params, BINGATE gate,
                                               ConstRingGSWACCKey& ek, ConstLWECiphertext& ct) const {
    if (params == nullptr)
        OPENFHE_THROW("BinFHECryptoParams is empty");
    if (ct == nullptr)
        OPENFHE_THROW("Ciphertext is empty");
    if (ek == nullptr)
        OPENFHE_THROW("Bootstrapping keys have not been generated. Please call BTKeyGen before calling bootstrapping.");

    // Specifies the range [lb, ub) that will be used for mapping
    NativeInteger q  = ct->GetModulus();
    auto qHalf       = q.ConvertToInt<uint32_t>() >> 1;
    auto& RGSWParams = params->GetRingGSWParams();
    NativeInteger q1 = RGSWParams->GetGateConst()[static_cast<size_t>(gate)];
    NativeInteger q2 = q1.ModAddFast(NativeInteger(qHalf), q);

    bool swap = q1 >= q2;
    auto lb = swap? q2 : q1;
    auto ub = swap? q1 : q2;

    // depending on whether the value is the range, it will be set
    // to either Q/8 or -Q/8 to match binary arithmetic
    auto& LWEParams      = params->GetLWEParams();
    NativeInteger Q      = LWEParams->GetQ();
    NativeInteger Q2p    = Q / (ct->GetptModulus() * 2) + 1;
    NativeInteger Q2pNeg = Q - Q2p;

    auto lv = swap? Q2p : Q2pNeg;
    auto uv = swap? Q2pNeg : Q2p;

    const uint32_t N = LWEParams->GetN();
    NativeVector m(N, Q);
    // Since q | (2*N), we deal with a sparse embedding of Z_Q[x]/(X^{q/2}+1) to
    // Z_Q[x]/(X^N+1)

    const uint32_t factor = (N / qHalf);

    NativeInteger b = ct->GetB();

    for (uint32_t i = 0; i < N; i += factor) {
        m[i] = ((b >= lb) && (b < ub)) ? lv : uv;
        b.ModSubFastEq(1, q);
    }

    std::vector<NativePoly> res(2);
    auto& polyParams = RGSWParams->GetPolyParams();
    // no need to do NTT as all coefficients of this poly are zero
    res[0] = NativePoly(polyParams, Format::EVALUATION, true);
    res[1] = NativePoly(polyParams, Format::COEFFICIENT, false);
    res[1].SetValues(std::move(m), Format::COEFFICIENT);
    res[1].SetFormat(Format::EVALUATION);

    // main accumulation computation
    // the following loop is the bottleneck of bootstrapping/binary gate
    // evaluation
    auto acc = std::make_shared<RLWECiphertextImpl>(std::move(res));
    ACCscheme->EvalAcc(RGSWParams, ek, acc, ct->GetA());
    return acc;
}

// Functions below are for large-precision sign evaluation,
// flooring, homomorphic digit decomposition, and arbitrary
// funciton evaluation, from https://eprint.iacr.org/2021/1337
template <typename Func>
RLWECiphertext BinFHEScheme::BootstrapFuncCore(const std::shared_ptr<BinFHECryptoParams>& params,
                                               ConstRingGSWACCKey& ek, ConstLWECiphertext& ct, const Func f,
                                               const NativeInteger& fmod) const {
    if (ek == nullptr) {
        std::string errMsg =
            "Bootstrapping keys have not been generated. Please call BTKeyGen before calling bootstrapping.";
        OPENFHE_THROW(errMsg);
    }

    auto& LWEParams  = params->GetLWEParams();
    auto& RGSWParams = params->GetRingGSWParams();
    auto polyParams  = RGSWParams->GetPolyParams();

    NativeInteger Q = LWEParams->GetQ();
    uint32_t N      = LWEParams->GetN();
    NativeVector m(N, Q);
    // For specific function evaluation instead of general bootstrapping
    NativeInteger ctMod    = ct->GetModulus();
    uint32_t factor        = (2 * N / ctMod.ConvertToInt());
    const NativeInteger& b = ct->GetB();
    for (size_t j = 0; j < (ctMod >> 1); ++j) {
        NativeInteger temp = b.ModSub(j, ctMod);
        m[j * factor]      = Q.ConvertToInt() / fmod.ConvertToInt() * f(temp, ctMod, fmod);
    }
    std::vector<NativePoly> res(2);
    // no need to do NTT as all coefficients of this poly are zero
    res[0] = NativePoly(polyParams, Format::EVALUATION, true);
    res[1] = NativePoly(polyParams, Format::COEFFICIENT, false);
    res[1].SetValues(std::move(m), Format::COEFFICIENT);
    res[1].SetFormat(Format::EVALUATION);

    // main accumulation computation
    // the following loop is the bottleneck of bootstrapping/binary gate
    // evaluation
    auto acc = std::make_shared<RLWECiphertextImpl>(std::move(res));
    ACCscheme->EvalAcc(RGSWParams, ek, acc, ct->GetA());
    return acc;
}

// Full evaluation as described in https://eprint.iacr.org/2020/086
template <typename Func>
LWECiphertext BinFHEScheme::BootstrapFunc(const std::shared_ptr<BinFHECryptoParams>& params, const RingGSWBTKey& EK,
                                          ConstLWECiphertext& ct, const Func f, const NativeInteger& fmod) const {
    // the accumulator result is encrypted w.r.t. the transposed secret key
    // we can transpose "a" to get an encryption under the original secret key
    auto accVec{BootstrapFuncCore(params, EK.BSkey, ct, f, fmod)->GetElements()};
    accVec[0] = accVec[0].Transpose();
    accVec[0].SetFormat(Format::COEFFICIENT);
    accVec[1].SetFormat(Format::COEFFICIENT);

    auto ctExt = std::make_shared<LWECiphertextImpl>(std::move(accVec[0].GetValues()), accVec[1][0]);

    auto& LWEParams = params->GetLWEParams();
    // Modulus switching to a middle step Q'
    auto ctMS = LWEscheme->ModSwitch(LWEParams->GetqKS(), ctExt);
    // Key switching
    auto ctKS = LWEscheme->KeySwitch(LWEParams, EK.KSkey, ctMS);
    // Modulus switching
    return LWEscheme->ModSwitch(fmod, ctKS);
}

};  // namespace lbcrypto
