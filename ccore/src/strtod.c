/* Exact decimal -> double (see strtod.h). Two steps, as Go's strconv.ParseFloat:
 * 1. Eisel-Lemire (Daniel Lemire, "Number Parsing at a Gigabyte per Second", 2021) on the first
 *    19 significant digits, with a 128-bit table of powers of ten. It answers for nearly every
 *    input, or says it cannot decide (halfway ambiguity, subnormals, overflow, or more than 19
 *    digits where the truncated and the next mantissa round differently).
 * 2. Otherwise the exact slow path: the decimal digits (up to 800 kept, a sticky flag for the
 *    rest) are shifted by powers of two until the value is in [0.5, 1), then 53 bits are taken
 *    and rounded half to even (Go's strconv/decimal.go, the "simple decimal conversion").
 * No libc, no __int128 (tcc and wasm32 have neither): 64x64 -> 128 products are done in 32-bit
 * halves. */
#include "strtod.h"
#include <stdint.h>
#include <string.h>

/* ---- the 128-bit table: floor(10^e normalized to [2^127, 2^128)), {lo, hi} ---- */
#define P10_MIN (-348)
#define P10_MAX 347
static const uint64_t P10[][2] = {
    {0x1732c869cd60e453u, 0xfa8fd5a0081c0288u}, /* 1e-348 */
    {0x0e7fbd42205c8eb4u, 0x9c99e58405118195u}, /* 1e-347 */
    {0x521fac92a873b261u, 0xc3c05ee50655e1fau}, /* 1e-346 */
    {0xe6a797b752909ef9u, 0xf4b0769e47eb5a78u}, /* 1e-345 */
    {0x9028bed2939a635cu, 0x98ee4a22ecf3188bu}, /* 1e-344 */
    {0x7432ee873880fc33u, 0xbf29dcaba82fdeaeu}, /* 1e-343 */
    {0x113faa2906a13b3fu, 0xeef453d6923bd65au}, /* 1e-342 */
    {0x4ac7ca59a424c507u, 0x9558b4661b6565f8u}, /* 1e-341 */
    {0x5d79bcf00d2df649u, 0xbaaee17fa23ebf76u}, /* 1e-340 */
    {0xf4d82c2c107973dcu, 0xe95a99df8ace6f53u}, /* 1e-339 */
    {0x79071b9b8a4be869u, 0x91d8a02bb6c10594u}, /* 1e-338 */
    {0x9748e2826cdee284u, 0xb64ec836a47146f9u}, /* 1e-337 */
    {0xfd1b1b2308169b25u, 0xe3e27a444d8d98b7u}, /* 1e-336 */
    {0xfe30f0f5e50e20f7u, 0x8e6d8c6ab0787f72u}, /* 1e-335 */
    {0xbdbd2d335e51a935u, 0xb208ef855c969f4fu}, /* 1e-334 */
    {0xad2c788035e61382u, 0xde8b2b66b3bc4723u}, /* 1e-333 */
    {0x4c3bcb5021afcc31u, 0x8b16fb203055ac76u}, /* 1e-332 */
    {0xdf4abe242a1bbf3du, 0xaddcb9e83c6b1793u}, /* 1e-331 */
    {0xd71d6dad34a2af0du, 0xd953e8624b85dd78u}, /* 1e-330 */
    {0x8672648c40e5ad68u, 0x87d4713d6f33aa6bu}, /* 1e-329 */
    {0x680efdaf511f18c2u, 0xa9c98d8ccb009506u}, /* 1e-328 */
    {0x0212bd1b2566def2u, 0xd43bf0effdc0ba48u}, /* 1e-327 */
    {0x014bb630f7604b57u, 0x84a57695fe98746du}, /* 1e-326 */
    {0x419ea3bd35385e2du, 0xa5ced43b7e3e9188u}, /* 1e-325 */
    {0x52064cac828675b9u, 0xcf42894a5dce35eau}, /* 1e-324 */
    {0x7343efebd1940993u, 0x818995ce7aa0e1b2u}, /* 1e-323 */
    {0x1014ebe6c5f90bf8u, 0xa1ebfb4219491a1fu}, /* 1e-322 */
    {0xd41a26e077774ef6u, 0xca66fa129f9b60a6u}, /* 1e-321 */
    {0x8920b098955522b4u, 0xfd00b897478238d0u}, /* 1e-320 */
    {0x55b46e5f5d5535b0u, 0x9e20735e8cb16382u}, /* 1e-319 */
    {0xeb2189f734aa831du, 0xc5a890362fddbc62u}, /* 1e-318 */
    {0xa5e9ec7501d523e4u, 0xf712b443bbd52b7bu}, /* 1e-317 */
    {0x47b233c92125366eu, 0x9a6bb0aa55653b2du}, /* 1e-316 */
    {0x999ec0bb696e840au, 0xc1069cd4eabe89f8u}, /* 1e-315 */
    {0xc00670ea43ca250du, 0xf148440a256e2c76u}, /* 1e-314 */
    {0x380406926a5e5728u, 0x96cd2a865764dbcau}, /* 1e-313 */
    {0xc605083704f5ecf2u, 0xbc807527ed3e12bcu}, /* 1e-312 */
    {0xf7864a44c633682eu, 0xeba09271e88d976bu}, /* 1e-311 */
    {0x7ab3ee6afbe0211du, 0x93445b8731587ea3u}, /* 1e-310 */
    {0x5960ea05bad82964u, 0xb8157268fdae9e4cu}, /* 1e-309 */
    {0x6fb92487298e33bdu, 0xe61acf033d1a45dfu}, /* 1e-308 */
    {0xa5d3b6d479f8e056u, 0x8fd0c16206306babu}, /* 1e-307 */
    {0x8f48a4899877186cu, 0xb3c4f1ba87bc8696u}, /* 1e-306 */
    {0x331acdabfe94de87u, 0xe0b62e2929aba83cu}, /* 1e-305 */
    {0x9ff0c08b7f1d0b14u, 0x8c71dcd9ba0b4925u}, /* 1e-304 */
    {0x07ecf0ae5ee44dd9u, 0xaf8e5410288e1b6fu}, /* 1e-303 */
    {0xc9e82cd9f69d6150u, 0xdb71e91432b1a24au}, /* 1e-302 */
    {0xbe311c083a225cd2u, 0x892731ac9faf056eu}, /* 1e-301 */
    {0x6dbd630a48aaf406u, 0xab70fe17c79ac6cau}, /* 1e-300 */
    {0x092cbbccdad5b108u, 0xd64d3d9db981787du}, /* 1e-299 */
    {0x25bbf56008c58ea5u, 0x85f0468293f0eb4eu}, /* 1e-298 */
    {0xaf2af2b80af6f24eu, 0xa76c582338ed2621u}, /* 1e-297 */
    {0x1af5af660db4aee1u, 0xd1476e2c07286faau}, /* 1e-296 */
    {0x50d98d9fc890ed4du, 0x82cca4db847945cau}, /* 1e-295 */
    {0xe50ff107bab528a0u, 0xa37fce126597973cu}, /* 1e-294 */
    {0x1e53ed49a96272c8u, 0xcc5fc196fefd7d0cu}, /* 1e-293 */
    {0x25e8e89c13bb0f7au, 0xff77b1fcbebcdc4fu}, /* 1e-292 */
    {0x77b191618c54e9acu, 0x9faacf3df73609b1u}, /* 1e-291 */
    {0xd59df5b9ef6a2417u, 0xc795830d75038c1du}, /* 1e-290 */
    {0x4b0573286b44ad1du, 0xf97ae3d0d2446f25u}, /* 1e-289 */
    {0x4ee367f9430aec32u, 0x9becce62836ac577u}, /* 1e-288 */
    {0x229c41f793cda73fu, 0xc2e801fb244576d5u}, /* 1e-287 */
    {0x6b43527578c1110fu, 0xf3a20279ed56d48au}, /* 1e-286 */
    {0x830a13896b78aaa9u, 0x9845418c345644d6u}, /* 1e-285 */
    {0x23cc986bc656d553u, 0xbe5691ef416bd60cu}, /* 1e-284 */
    {0x2cbfbe86b7ec8aa8u, 0xedec366b11c6cb8fu}, /* 1e-283 */
    {0x7bf7d71432f3d6a9u, 0x94b3a202eb1c3f39u}, /* 1e-282 */
    {0xdaf5ccd93fb0cc53u, 0xb9e08a83a5e34f07u}, /* 1e-281 */
    {0xd1b3400f8f9cff68u, 0xe858ad248f5c22c9u}, /* 1e-280 */
    {0x23100809b9c21fa1u, 0x91376c36d99995beu}, /* 1e-279 */
    {0xabd40a0c2832a78au, 0xb58547448ffffb2du}, /* 1e-278 */
    {0x16c90c8f323f516cu, 0xe2e69915b3fff9f9u}, /* 1e-277 */
    {0xae3da7d97f6792e3u, 0x8dd01fad907ffc3bu}, /* 1e-276 */
    {0x99cd11cfdf41779cu, 0xb1442798f49ffb4au}, /* 1e-275 */
    {0x40405643d711d583u, 0xdd95317f31c7fa1du}, /* 1e-274 */
    {0x482835ea666b2572u, 0x8a7d3eef7f1cfc52u}, /* 1e-273 */
    {0xda3243650005eecfu, 0xad1c8eab5ee43b66u}, /* 1e-272 */
    {0x90bed43e40076a82u, 0xd863b256369d4a40u}, /* 1e-271 */
    {0x5a7744a6e804a291u, 0x873e4f75e2224e68u}, /* 1e-270 */
    {0x711515d0a205cb36u, 0xa90de3535aaae202u}, /* 1e-269 */
    {0x0d5a5b44ca873e03u, 0xd3515c2831559a83u}, /* 1e-268 */
    {0xe858790afe9486c2u, 0x8412d9991ed58091u}, /* 1e-267 */
    {0x626e974dbe39a872u, 0xa5178fff668ae0b6u}, /* 1e-266 */
    {0xfb0a3d212dc8128fu, 0xce5d73ff402d98e3u}, /* 1e-265 */
    {0x7ce66634bc9d0b99u, 0x80fa687f881c7f8eu}, /* 1e-264 */
    {0x1c1fffc1ebc44e80u, 0xa139029f6a239f72u}, /* 1e-263 */
    {0xa327ffb266b56220u, 0xc987434744ac874eu}, /* 1e-262 */
    {0x4bf1ff9f0062baa8u, 0xfbe9141915d7a922u}, /* 1e-261 */
    {0x6f773fc3603db4a9u, 0x9d71ac8fada6c9b5u}, /* 1e-260 */
    {0xcb550fb4384d21d3u, 0xc4ce17b399107c22u}, /* 1e-259 */
    {0x7e2a53a146606a48u, 0xf6019da07f549b2bu}, /* 1e-258 */
    {0x2eda7444cbfc426du, 0x99c102844f94e0fbu}, /* 1e-257 */
    {0xfa911155fefb5308u, 0xc0314325637a1939u}, /* 1e-256 */
    {0x793555ab7eba27cau, 0xf03d93eebc589f88u}, /* 1e-255 */
    {0x4bc1558b2f3458deu, 0x96267c7535b763b5u}, /* 1e-254 */
    {0x9eb1aaedfb016f16u, 0xbbb01b9283253ca2u}, /* 1e-253 */
    {0x465e15a979c1cadcu, 0xea9c227723ee8bcbu}, /* 1e-252 */
    {0x0bfacd89ec191ec9u, 0x92a1958a7675175fu}, /* 1e-251 */
    {0xcef980ec671f667bu, 0xb749faed14125d36u}, /* 1e-250 */
    {0x82b7e12780e7401au, 0xe51c79a85916f484u}, /* 1e-249 */
    {0xd1b2ecb8b0908810u, 0x8f31cc0937ae58d2u}, /* 1e-248 */
    {0x861fa7e6dcb4aa15u, 0xb2fe3f0b8599ef07u}, /* 1e-247 */
    {0x67a791e093e1d49au, 0xdfbdcece67006ac9u}, /* 1e-246 */
    {0xe0c8bb2c5c6d24e0u, 0x8bd6a141006042bdu}, /* 1e-245 */
    {0x58fae9f773886e18u, 0xaecc49914078536du}, /* 1e-244 */
    {0xaf39a475506a899eu, 0xda7f5bf590966848u}, /* 1e-243 */
    {0x6d8406c952429603u, 0x888f99797a5e012du}, /* 1e-242 */
    {0xc8e5087ba6d33b83u, 0xaab37fd7d8f58178u}, /* 1e-241 */
    {0xfb1e4a9a90880a64u, 0xd5605fcdcf32e1d6u}, /* 1e-240 */
    {0x5cf2eea09a55067fu, 0x855c3be0a17fcd26u}, /* 1e-239 */
    {0xf42faa48c0ea481eu, 0xa6b34ad8c9dfc06fu}, /* 1e-238 */
    {0xf13b94daf124da26u, 0xd0601d8efc57b08bu}, /* 1e-237 */
    {0x76c53d08d6b70858u, 0x823c12795db6ce57u}, /* 1e-236 */
    {0x54768c4b0c64ca6eu, 0xa2cb1717b52481edu}, /* 1e-235 */
    {0xa9942f5dcf7dfd09u, 0xcb7ddcdda26da268u}, /* 1e-234 */
    {0xd3f93b35435d7c4cu, 0xfe5d54150b090b02u}, /* 1e-233 */
    {0xc47bc5014a1a6dafu, 0x9efa548d26e5a6e1u}, /* 1e-232 */
    {0x359ab6419ca1091bu, 0xc6b8e9b0709f109au}, /* 1e-231 */
    {0xc30163d203c94b62u, 0xf867241c8cc6d4c0u}, /* 1e-230 */
    {0x79e0de63425dcf1du, 0x9b407691d7fc44f8u}, /* 1e-229 */
    {0x985915fc12f542e4u, 0xc21094364dfb5636u}, /* 1e-228 */
    {0x3e6f5b7b17b2939du, 0xf294b943e17a2bc4u}, /* 1e-227 */
    {0xa705992ceecf9c42u, 0x979cf3ca6cec5b5au}, /* 1e-226 */
    {0x50c6ff782a838353u, 0xbd8430bd08277231u}, /* 1e-225 */
    {0xa4f8bf5635246428u, 0xece53cec4a314ebdu}, /* 1e-224 */
    {0x871b7795e136be99u, 0x940f4613ae5ed136u}, /* 1e-223 */
    {0x28e2557b59846e3fu, 0xb913179899f68584u}, /* 1e-222 */
    {0x331aeada2fe589cfu, 0xe757dd7ec07426e5u}, /* 1e-221 */
    {0x3ff0d2c85def7621u, 0x9096ea6f3848984fu}, /* 1e-220 */
    {0x0fed077a756b53a9u, 0xb4bca50b065abe63u}, /* 1e-219 */
    {0xd3e8495912c62894u, 0xe1ebce4dc7f16dfbu}, /* 1e-218 */
    {0x64712dd7abbbd95cu, 0x8d3360f09cf6e4bdu}, /* 1e-217 */
    {0xbd8d794d96aacfb3u, 0xb080392cc4349decu}, /* 1e-216 */
    {0xecf0d7a0fc5583a0u, 0xdca04777f541c567u}, /* 1e-215 */
    {0xf41686c49db57244u, 0x89e42caaf9491b60u}, /* 1e-214 */
    {0x311c2875c522ced5u, 0xac5d37d5b79b6239u}, /* 1e-213 */
    {0x7d633293366b828bu, 0xd77485cb25823ac7u}, /* 1e-212 */
    {0xae5dff9c02033197u, 0x86a8d39ef77164bcu}, /* 1e-211 */
    {0xd9f57f830283fdfcu, 0xa8530886b54dbdebu}, /* 1e-210 */
    {0xd072df63c324fd7bu, 0xd267caa862a12d66u}, /* 1e-209 */
    {0x4247cb9e59f71e6du, 0x8380dea93da4bc60u}, /* 1e-208 */
    {0x52d9be85f074e608u, 0xa46116538d0deb78u}, /* 1e-207 */
    {0x67902e276c921f8bu, 0xcd795be870516656u}, /* 1e-206 */
    {0x00ba1cd8a3db53b6u, 0x806bd9714632dff6u}, /* 1e-205 */
    {0x80e8a40eccd228a4u, 0xa086cfcd97bf97f3u}, /* 1e-204 */
    {0x6122cd128006b2cdu, 0xc8a883c0fdaf7df0u}, /* 1e-203 */
    {0x796b805720085f81u, 0xfad2a4b13d1b5d6cu}, /* 1e-202 */
    {0xcbe3303674053bb0u, 0x9cc3a6eec6311a63u}, /* 1e-201 */
    {0xbedbfc4411068a9cu, 0xc3f490aa77bd60fcu}, /* 1e-200 */
    {0xee92fb5515482d44u, 0xf4f1b4d515acb93bu}, /* 1e-199 */
    {0x751bdd152d4d1c4au, 0x991711052d8bf3c5u}, /* 1e-198 */
    {0xd262d45a78a0635du, 0xbf5cd54678eef0b6u}, /* 1e-197 */
    {0x86fb897116c87c34u, 0xef340a98172aace4u}, /* 1e-196 */
    {0xd45d35e6ae3d4da0u, 0x9580869f0e7aac0eu}, /* 1e-195 */
    {0x8974836059cca109u, 0xbae0a846d2195712u}, /* 1e-194 */
    {0x2bd1a438703fc94bu, 0xe998d258869facd7u}, /* 1e-193 */
    {0x7b6306a34627ddcfu, 0x91ff83775423cc06u}, /* 1e-192 */
    {0x1a3bc84c17b1d542u, 0xb67f6455292cbf08u}, /* 1e-191 */
    {0x20caba5f1d9e4a93u, 0xe41f3d6a7377eecau}, /* 1e-190 */
    {0x547eb47b7282ee9cu, 0x8e938662882af53eu}, /* 1e-189 */
    {0xe99e619a4f23aa43u, 0xb23867fb2a35b28du}, /* 1e-188 */
    {0x6405fa00e2ec94d4u, 0xdec681f9f4c31f31u}, /* 1e-187 */
    {0xde83bc408dd3dd04u, 0x8b3c113c38f9f37eu}, /* 1e-186 */
    {0x9624ab50b148d445u, 0xae0b158b4738705eu}, /* 1e-185 */
    {0x3badd624dd9b0957u, 0xd98ddaee19068c76u}, /* 1e-184 */
    {0xe54ca5d70a80e5d6u, 0x87f8a8d4cfa417c9u}, /* 1e-183 */
    {0x5e9fcf4ccd211f4cu, 0xa9f6d30a038d1dbcu}, /* 1e-182 */
    {0x7647c3200069671fu, 0xd47487cc8470652bu}, /* 1e-181 */
    {0x29ecd9f40041e073u, 0x84c8d4dfd2c63f3bu}, /* 1e-180 */
    {0xf468107100525890u, 0xa5fb0a17c777cf09u}, /* 1e-179 */
    {0x7182148d4066eeb4u, 0xcf79cc9db955c2ccu}, /* 1e-178 */
    {0xc6f14cd848405530u, 0x81ac1fe293d599bfu}, /* 1e-177 */
    {0xb8ada00e5a506a7cu, 0xa21727db38cb002fu}, /* 1e-176 */
    {0xa6d90811f0e4851cu, 0xca9cf1d206fdc03bu}, /* 1e-175 */
    {0x908f4a166d1da663u, 0xfd442e4688bd304au}, /* 1e-174 */
    {0x9a598e4e043287feu, 0x9e4a9cec15763e2eu}, /* 1e-173 */
    {0x40eff1e1853f29fdu, 0xc5dd44271ad3cdbau}, /* 1e-172 */
    {0xd12bee59e68ef47cu, 0xf7549530e188c128u}, /* 1e-171 */
    {0x82bb74f8301958ceu, 0x9a94dd3e8cf578b9u}, /* 1e-170 */
    {0xe36a52363c1faf01u, 0xc13a148e3032d6e7u}, /* 1e-169 */
    {0xdc44e6c3cb279ac1u, 0xf18899b1bc3f8ca1u}, /* 1e-168 */
    {0x29ab103a5ef8c0b9u, 0x96f5600f15a7b7e5u}, /* 1e-167 */
    {0x7415d448f6b6f0e7u, 0xbcb2b812db11a5deu}, /* 1e-166 */
    {0x111b495b3464ad21u, 0xebdf661791d60f56u}, /* 1e-165 */
    {0xcab10dd900beec34u, 0x936b9fcebb25c995u}, /* 1e-164 */
    {0x3d5d514f40eea742u, 0xb84687c269ef3bfbu}, /* 1e-163 */
    {0x0cb4a5a3112a5112u, 0xe65829b3046b0afau}, /* 1e-162 */
    {0x47f0e785eaba72abu, 0x8ff71a0fe2c2e6dcu}, /* 1e-161 */
    {0x59ed216765690f56u, 0xb3f4e093db73a093u}, /* 1e-160 */
    {0x306869c13ec3532cu, 0xe0f218b8d25088b8u}, /* 1e-159 */
    {0x1e414218c73a13fbu, 0x8c974f7383725573u}, /* 1e-158 */
    {0xe5d1929ef90898fau, 0xafbd2350644eeacfu}, /* 1e-157 */
    {0xdf45f746b74abf39u, 0xdbac6c247d62a583u}, /* 1e-156 */
    {0x6b8bba8c328eb783u, 0x894bc396ce5da772u}, /* 1e-155 */
    {0x066ea92f3f326564u, 0xab9eb47c81f5114fu}, /* 1e-154 */
    {0xc80a537b0efefebdu, 0xd686619ba27255a2u}, /* 1e-153 */
    {0xbd06742ce95f5f36u, 0x8613fd0145877585u}, /* 1e-152 */
    {0x2c48113823b73704u, 0xa798fc4196e952e7u}, /* 1e-151 */
    {0xf75a15862ca504c5u, 0xd17f3b51fca3a7a0u}, /* 1e-150 */
    {0x9a984d73dbe722fbu, 0x82ef85133de648c4u}, /* 1e-149 */
    {0xc13e60d0d2e0ebbau, 0xa3ab66580d5fdaf5u}, /* 1e-148 */
    {0x318df905079926a8u, 0xcc963fee10b7d1b3u}, /* 1e-147 */
    {0xfdf17746497f7052u, 0xffbbcfe994e5c61fu}, /* 1e-146 */
    {0xfeb6ea8bedefa633u, 0x9fd561f1fd0f9bd3u}, /* 1e-145 */
    {0xfe64a52ee96b8fc0u, 0xc7caba6e7c5382c8u}, /* 1e-144 */
    {0x3dfdce7aa3c673b0u, 0xf9bd690a1b68637bu}, /* 1e-143 */
    {0x06bea10ca65c084eu, 0x9c1661a651213e2du}, /* 1e-142 */
    {0x486e494fcff30a62u, 0xc31bfa0fe5698db8u}, /* 1e-141 */
    {0x5a89dba3c3efccfau, 0xf3e2f893dec3f126u}, /* 1e-140 */
    {0xf89629465a75e01cu, 0x986ddb5c6b3a76b7u}, /* 1e-139 */
    {0xf6bbb397f1135823u, 0xbe89523386091465u}, /* 1e-138 */
    {0x746aa07ded582e2cu, 0xee2ba6c0678b597fu}, /* 1e-137 */
    {0xa8c2a44eb4571cdcu, 0x94db483840b717efu}, /* 1e-136 */
    {0x92f34d62616ce413u, 0xba121a4650e4ddebu}, /* 1e-135 */
    {0x77b020baf9c81d17u, 0xe896a0d7e51e1566u}, /* 1e-134 */
    {0x0ace1474dc1d122eu, 0x915e2486ef32cd60u}, /* 1e-133 */
    {0x0d819992132456bau, 0xb5b5ada8aaff80b8u}, /* 1e-132 */
    {0x10e1fff697ed6c69u, 0xe3231912d5bf60e6u}, /* 1e-131 */
    {0xca8d3ffa1ef463c1u, 0x8df5efabc5979c8fu}, /* 1e-130 */
    {0xbd308ff8a6b17cb2u, 0xb1736b96b6fd83b3u}, /* 1e-129 */
    {0xac7cb3f6d05ddbdeu, 0xddd0467c64bce4a0u}, /* 1e-128 */
    {0x6bcdf07a423aa96bu, 0x8aa22c0dbef60ee4u}, /* 1e-127 */
    {0x86c16c98d2c953c6u, 0xad4ab7112eb3929du}, /* 1e-126 */
    {0xe871c7bf077ba8b7u, 0xd89d64d57a607744u}, /* 1e-125 */
    {0x11471cd764ad4972u, 0x87625f056c7c4a8bu}, /* 1e-124 */
    {0xd598e40d3dd89bcfu, 0xa93af6c6c79b5d2du}, /* 1e-123 */
    {0x4aff1d108d4ec2c3u, 0xd389b47879823479u}, /* 1e-122 */
    {0xcedf722a585139bau, 0x843610cb4bf160cbu}, /* 1e-121 */
    {0xc2974eb4ee658828u, 0xa54394fe1eedb8feu}, /* 1e-120 */
    {0x733d226229feea32u, 0xce947a3da6a9273eu}, /* 1e-119 */
    {0x0806357d5a3f525fu, 0x811ccc668829b887u}, /* 1e-118 */
    {0xca07c2dcb0cf26f7u, 0xa163ff802a3426a8u}, /* 1e-117 */
    {0xfc89b393dd02f0b5u, 0xc9bcff6034c13052u}, /* 1e-116 */
    {0xbbac2078d443ace2u, 0xfc2c3f3841f17c67u}, /* 1e-115 */
    {0xd54b944b84aa4c0du, 0x9d9ba7832936edc0u}, /* 1e-114 */
    {0x0a9e795e65d4df11u, 0xc5029163f384a931u}, /* 1e-113 */
    {0x4d4617b5ff4a16d5u, 0xf64335bcf065d37du}, /* 1e-112 */
    {0x504bced1bf8e4e45u, 0x99ea0196163fa42eu}, /* 1e-111 */
    {0xe45ec2862f71e1d6u, 0xc06481fb9bcf8d39u}, /* 1e-110 */
    {0x5d767327bb4e5a4cu, 0xf07da27a82c37088u}, /* 1e-109 */
    {0x3a6a07f8d510f86fu, 0x964e858c91ba2655u}, /* 1e-108 */
    {0x890489f70a55368bu, 0xbbe226efb628afeau}, /* 1e-107 */
    {0x2b45ac74ccea842eu, 0xeadab0aba3b2dbe5u}, /* 1e-106 */
    {0x3b0b8bc90012929du, 0x92c8ae6b464fc96fu}, /* 1e-105 */
    {0x09ce6ebb40173744u, 0xb77ada0617e3bbcbu}, /* 1e-104 */
    {0xcc420a6a101d0515u, 0xe55990879ddcaabdu}, /* 1e-103 */
    {0x9fa946824a12232du, 0x8f57fa54c2a9eab6u}, /* 1e-102 */
    {0x47939822dc96abf9u, 0xb32df8e9f3546564u}, /* 1e-101 */
    {0x59787e2b93bc56f7u, 0xdff9772470297ebdu}, /* 1e-100 */
    {0x57eb4edb3c55b65au, 0x8bfbea76c619ef36u}, /* 1e-99 */
    {0xede622920b6b23f1u, 0xaefae51477a06b03u}, /* 1e-98 */
    {0xe95fab368e45ecedu, 0xdab99e59958885c4u}, /* 1e-97 */
    {0x11dbcb0218ebb414u, 0x88b402f7fd75539bu}, /* 1e-96 */
    {0xd652bdc29f26a119u, 0xaae103b5fcd2a881u}, /* 1e-95 */
    {0x4be76d3346f0495fu, 0xd59944a37c0752a2u}, /* 1e-94 */
    {0x6f70a4400c562ddbu, 0x857fcae62d8493a5u}, /* 1e-93 */
    {0xcb4ccd500f6bb952u, 0xa6dfbd9fb8e5b88eu}, /* 1e-92 */
    {0x7e2000a41346a7a7u, 0xd097ad07a71f26b2u}, /* 1e-91 */
    {0x8ed400668c0c28c8u, 0x825ecc24c873782fu}, /* 1e-90 */
    {0x728900802f0f32fau, 0xa2f67f2dfa90563bu}, /* 1e-89 */
    {0x4f2b40a03ad2ffb9u, 0xcbb41ef979346bcau}, /* 1e-88 */
    {0xe2f610c84987bfa8u, 0xfea126b7d78186bcu}, /* 1e-87 */
    {0x0dd9ca7d2df4d7c9u, 0x9f24b832e6b0f436u}, /* 1e-86 */
    {0x91503d1c79720dbbu, 0xc6ede63fa05d3143u}, /* 1e-85 */
    {0x75a44c6397ce912au, 0xf8a95fcf88747d94u}, /* 1e-84 */
    {0xc986afbe3ee11abau, 0x9b69dbe1b548ce7cu}, /* 1e-83 */
    {0xfbe85badce996168u, 0xc24452da229b021bu}, /* 1e-82 */
    {0xfae27299423fb9c3u, 0xf2d56790ab41c2a2u}, /* 1e-81 */
    {0xdccd879fc967d41au, 0x97c560ba6b0919a5u}, /* 1e-80 */
    {0x5400e987bbc1c920u, 0xbdb6b8e905cb600fu}, /* 1e-79 */
    {0x290123e9aab23b68u, 0xed246723473e3813u}, /* 1e-78 */
    {0xf9a0b6720aaf6521u, 0x9436c0760c86e30bu}, /* 1e-77 */
    {0xf808e40e8d5b3e69u, 0xb94470938fa89bceu}, /* 1e-76 */
    {0xb60b1d1230b20e04u, 0xe7958cb87392c2c2u}, /* 1e-75 */
    {0xb1c6f22b5e6f48c2u, 0x90bd77f3483bb9b9u}, /* 1e-74 */
    {0x1e38aeb6360b1af3u, 0xb4ecd5f01a4aa828u}, /* 1e-73 */
    {0x25c6da63c38de1b0u, 0xe2280b6c20dd5232u}, /* 1e-72 */
    {0x579c487e5a38ad0eu, 0x8d590723948a535fu}, /* 1e-71 */
    {0x2d835a9df0c6d851u, 0xb0af48ec79ace837u}, /* 1e-70 */
    {0xf8e431456cf88e65u, 0xdcdb1b2798182244u}, /* 1e-69 */
    {0x1b8e9ecb641b58ffu, 0x8a08f0f8bf0f156bu}, /* 1e-68 */
    {0xe272467e3d222f3fu, 0xac8b2d36eed2dac5u}, /* 1e-67 */
    {0x5b0ed81dcc6abb0fu, 0xd7adf884aa879177u}, /* 1e-66 */
    {0x98e947129fc2b4e9u, 0x86ccbb52ea94baeau}, /* 1e-65 */
    {0x3f2398d747b36224u, 0xa87fea27a539e9a5u}, /* 1e-64 */
    {0x8eec7f0d19a03aadu, 0xd29fe4b18e88640eu}, /* 1e-63 */
    {0x1953cf68300424acu, 0x83a3eeeef9153e89u}, /* 1e-62 */
    {0x5fa8c3423c052dd7u, 0xa48ceaaab75a8e2bu}, /* 1e-61 */
    {0x3792f412cb06794du, 0xcdb02555653131b6u}, /* 1e-60 */
    {0xe2bbd88bbee40bd0u, 0x808e17555f3ebf11u}, /* 1e-59 */
    {0x5b6aceaeae9d0ec4u, 0xa0b19d2ab70e6ed6u}, /* 1e-58 */
    {0xf245825a5a445275u, 0xc8de047564d20a8bu}, /* 1e-57 */
    {0xeed6e2f0f0d56712u, 0xfb158592be068d2eu}, /* 1e-56 */
    {0x55464dd69685606bu, 0x9ced737bb6c4183du}, /* 1e-55 */
    {0xaa97e14c3c26b886u, 0xc428d05aa4751e4cu}, /* 1e-54 */
    {0xd53dd99f4b3066a8u, 0xf53304714d9265dfu}, /* 1e-53 */
    {0xe546a8038efe4029u, 0x993fe2c6d07b7fabu}, /* 1e-52 */
    {0xde98520472bdd033u, 0xbf8fdb78849a5f96u}, /* 1e-51 */
    {0x963e66858f6d4440u, 0xef73d256a5c0f77cu}, /* 1e-50 */
    {0xdde7001379a44aa8u, 0x95a8637627989aadu}, /* 1e-49 */
    {0x5560c018580d5d52u, 0xbb127c53b17ec159u}, /* 1e-48 */
    {0xaab8f01e6e10b4a6u, 0xe9d71b689dde71afu}, /* 1e-47 */
    {0xcab3961304ca70e8u, 0x9226712162ab070du}, /* 1e-46 */
    {0x3d607b97c5fd0d22u, 0xb6b00d69bb55c8d1u}, /* 1e-45 */
    {0x8cb89a7db77c506au, 0xe45c10c42a2b3b05u}, /* 1e-44 */
    {0x77f3608e92adb242u, 0x8eb98a7a9a5b04e3u}, /* 1e-43 */
    {0x55f038b237591ed3u, 0xb267ed1940f1c61cu}, /* 1e-42 */
    {0x6b6c46dec52f6688u, 0xdf01e85f912e37a3u}, /* 1e-41 */
    {0x2323ac4b3b3da015u, 0x8b61313bbabce2c6u}, /* 1e-40 */
    {0xabec975e0a0d081au, 0xae397d8aa96c1b77u}, /* 1e-39 */
    {0x96e7bd358c904a21u, 0xd9c7dced53c72255u}, /* 1e-38 */
    {0x7e50d64177da2e54u, 0x881cea14545c7575u}, /* 1e-37 */
    {0xdde50bd1d5d0b9e9u, 0xaa242499697392d2u}, /* 1e-36 */
    {0x955e4ec64b44e864u, 0xd4ad2dbfc3d07787u}, /* 1e-35 */
    {0xbd5af13bef0b113eu, 0x84ec3c97da624ab4u}, /* 1e-34 */
    {0xecb1ad8aeacdd58eu, 0xa6274bbdd0fadd61u}, /* 1e-33 */
    {0x67de18eda5814af2u, 0xcfb11ead453994bau}, /* 1e-32 */
    {0x80eacf948770ced7u, 0x81ceb32c4b43fcf4u}, /* 1e-31 */
    {0xa1258379a94d028du, 0xa2425ff75e14fc31u}, /* 1e-30 */
    {0x096ee45813a04330u, 0xcad2f7f5359a3b3eu}, /* 1e-29 */
    {0x8bca9d6e188853fcu, 0xfd87b5f28300ca0du}, /* 1e-28 */
    {0x775ea264cf55347du, 0x9e74d1b791e07e48u}, /* 1e-27 */
    {0x95364afe032a819du, 0xc612062576589ddau}, /* 1e-26 */
    {0x3a83ddbd83f52204u, 0xf79687aed3eec551u}, /* 1e-25 */
    {0xc4926a9672793542u, 0x9abe14cd44753b52u}, /* 1e-24 */
    {0x75b7053c0f178293u, 0xc16d9a0095928a27u}, /* 1e-23 */
    {0x5324c68b12dd6338u, 0xf1c90080baf72cb1u}, /* 1e-22 */
    {0xd3f6fc16ebca5e03u, 0x971da05074da7beeu}, /* 1e-21 */
    {0x88f4bb1ca6bcf584u, 0xbce5086492111aeau}, /* 1e-20 */
    {0x2b31e9e3d06c32e5u, 0xec1e4a7db69561a5u}, /* 1e-19 */
    {0x3aff322e62439fcfu, 0x9392ee8e921d5d07u}, /* 1e-18 */
    {0x09befeb9fad487c2u, 0xb877aa3236a4b449u}, /* 1e-17 */
    {0x4c2ebe687989a9b3u, 0xe69594bec44de15bu}, /* 1e-16 */
    {0x0f9d37014bf60a10u, 0x901d7cf73ab0acd9u}, /* 1e-15 */
    {0x538484c19ef38c94u, 0xb424dc35095cd80fu}, /* 1e-14 */
    {0x2865a5f206b06fb9u, 0xe12e13424bb40e13u}, /* 1e-13 */
    {0xf93f87b7442e45d3u, 0x8cbccc096f5088cbu}, /* 1e-12 */
    {0xf78f69a51539d748u, 0xafebff0bcb24aafeu}, /* 1e-11 */
    {0xb573440e5a884d1bu, 0xdbe6fecebdedd5beu}, /* 1e-10 */
    {0x31680a88f8953030u, 0x89705f4136b4a597u}, /* 1e-9 */
    {0xfdc20d2b36ba7c3du, 0xabcc77118461cefcu}, /* 1e-8 */
    {0x3d32907604691b4cu, 0xd6bf94d5e57a42bcu}, /* 1e-7 */
    {0xa63f9a49c2c1b10fu, 0x8637bd05af6c69b5u}, /* 1e-6 */
    {0x0fcf80dc33721d53u, 0xa7c5ac471b478423u}, /* 1e-5 */
    {0xd3c36113404ea4a8u, 0xd1b71758e219652bu}, /* 1e-4 */
    {0x645a1cac083126e9u, 0x83126e978d4fdf3bu}, /* 1e-3 */
    {0x3d70a3d70a3d70a3u, 0xa3d70a3d70a3d70au}, /* 1e-2 */
    {0xccccccccccccccccu, 0xccccccccccccccccu}, /* 1e-1 */
    {0x0000000000000000u, 0x8000000000000000u}, /* 1e0 */
    {0x0000000000000000u, 0xa000000000000000u}, /* 1e1 */
    {0x0000000000000000u, 0xc800000000000000u}, /* 1e2 */
    {0x0000000000000000u, 0xfa00000000000000u}, /* 1e3 */
    {0x0000000000000000u, 0x9c40000000000000u}, /* 1e4 */
    {0x0000000000000000u, 0xc350000000000000u}, /* 1e5 */
    {0x0000000000000000u, 0xf424000000000000u}, /* 1e6 */
    {0x0000000000000000u, 0x9896800000000000u}, /* 1e7 */
    {0x0000000000000000u, 0xbebc200000000000u}, /* 1e8 */
    {0x0000000000000000u, 0xee6b280000000000u}, /* 1e9 */
    {0x0000000000000000u, 0x9502f90000000000u}, /* 1e10 */
    {0x0000000000000000u, 0xba43b74000000000u}, /* 1e11 */
    {0x0000000000000000u, 0xe8d4a51000000000u}, /* 1e12 */
    {0x0000000000000000u, 0x9184e72a00000000u}, /* 1e13 */
    {0x0000000000000000u, 0xb5e620f480000000u}, /* 1e14 */
    {0x0000000000000000u, 0xe35fa931a0000000u}, /* 1e15 */
    {0x0000000000000000u, 0x8e1bc9bf04000000u}, /* 1e16 */
    {0x0000000000000000u, 0xb1a2bc2ec5000000u}, /* 1e17 */
    {0x0000000000000000u, 0xde0b6b3a76400000u}, /* 1e18 */
    {0x0000000000000000u, 0x8ac7230489e80000u}, /* 1e19 */
    {0x0000000000000000u, 0xad78ebc5ac620000u}, /* 1e20 */
    {0x0000000000000000u, 0xd8d726b7177a8000u}, /* 1e21 */
    {0x0000000000000000u, 0x878678326eac9000u}, /* 1e22 */
    {0x0000000000000000u, 0xa968163f0a57b400u}, /* 1e23 */
    {0x0000000000000000u, 0xd3c21bcecceda100u}, /* 1e24 */
    {0x0000000000000000u, 0x84595161401484a0u}, /* 1e25 */
    {0x0000000000000000u, 0xa56fa5b99019a5c8u}, /* 1e26 */
    {0x0000000000000000u, 0xcecb8f27f4200f3au}, /* 1e27 */
    {0x4000000000000000u, 0x813f3978f8940984u}, /* 1e28 */
    {0x5000000000000000u, 0xa18f07d736b90be5u}, /* 1e29 */
    {0xa400000000000000u, 0xc9f2c9cd04674edeu}, /* 1e30 */
    {0x4d00000000000000u, 0xfc6f7c4045812296u}, /* 1e31 */
    {0xf020000000000000u, 0x9dc5ada82b70b59du}, /* 1e32 */
    {0x6c28000000000000u, 0xc5371912364ce305u}, /* 1e33 */
    {0xc732000000000000u, 0xf684df56c3e01bc6u}, /* 1e34 */
    {0x3c7f400000000000u, 0x9a130b963a6c115cu}, /* 1e35 */
    {0x4b9f100000000000u, 0xc097ce7bc90715b3u}, /* 1e36 */
    {0x1e86d40000000000u, 0xf0bdc21abb48db20u}, /* 1e37 */
    {0x1314448000000000u, 0x96769950b50d88f4u}, /* 1e38 */
    {0x17d955a000000000u, 0xbc143fa4e250eb31u}, /* 1e39 */
    {0x5dcfab0800000000u, 0xeb194f8e1ae525fdu}, /* 1e40 */
    {0x5aa1cae500000000u, 0x92efd1b8d0cf37beu}, /* 1e41 */
    {0xf14a3d9e40000000u, 0xb7abc627050305adu}, /* 1e42 */
    {0x6d9ccd05d0000000u, 0xe596b7b0c643c719u}, /* 1e43 */
    {0xe4820023a2000000u, 0x8f7e32ce7bea5c6fu}, /* 1e44 */
    {0xdda2802c8a800000u, 0xb35dbf821ae4f38bu}, /* 1e45 */
    {0xd50b2037ad200000u, 0xe0352f62a19e306eu}, /* 1e46 */
    {0x4526f422cc340000u, 0x8c213d9da502de45u}, /* 1e47 */
    {0x9670b12b7f410000u, 0xaf298d050e4395d6u}, /* 1e48 */
    {0x3c0cdd765f114000u, 0xdaf3f04651d47b4cu}, /* 1e49 */
    {0xa5880a69fb6ac800u, 0x88d8762bf324cd0fu}, /* 1e50 */
    {0x8eea0d047a457a00u, 0xab0e93b6efee0053u}, /* 1e51 */
    {0x72a4904598d6d880u, 0xd5d238a4abe98068u}, /* 1e52 */
    {0x47a6da2b7f864750u, 0x85a36366eb71f041u}, /* 1e53 */
    {0x999090b65f67d924u, 0xa70c3c40a64e6c51u}, /* 1e54 */
    {0xfff4b4e3f741cf6du, 0xd0cf4b50cfe20765u}, /* 1e55 */
    {0xbff8f10e7a8921a4u, 0x82818f1281ed449fu}, /* 1e56 */
    {0xaff72d52192b6a0du, 0xa321f2d7226895c7u}, /* 1e57 */
    {0x9bf4f8a69f764490u, 0xcbea6f8ceb02bb39u}, /* 1e58 */
    {0x02f236d04753d5b4u, 0xfee50b7025c36a08u}, /* 1e59 */
    {0x01d762422c946590u, 0x9f4f2726179a2245u}, /* 1e60 */
    {0x424d3ad2b7b97ef5u, 0xc722f0ef9d80aad6u}, /* 1e61 */
    {0xd2e0898765a7deb2u, 0xf8ebad2b84e0d58bu}, /* 1e62 */
    {0x63cc55f49f88eb2fu, 0x9b934c3b330c8577u}, /* 1e63 */
    {0x3cbf6b71c76b25fbu, 0xc2781f49ffcfa6d5u}, /* 1e64 */
    {0x8bef464e3945ef7au, 0xf316271c7fc3908au}, /* 1e65 */
    {0x97758bf0e3cbb5acu, 0x97edd871cfda3a56u}, /* 1e66 */
    {0x3d52eeed1cbea317u, 0xbde94e8e43d0c8ecu}, /* 1e67 */
    {0x4ca7aaa863ee4bddu, 0xed63a231d4c4fb27u}, /* 1e68 */
    {0x8fe8caa93e74ef6au, 0x945e455f24fb1cf8u}, /* 1e69 */
    {0xb3e2fd538e122b44u, 0xb975d6b6ee39e436u}, /* 1e70 */
    {0x60dbbca87196b616u, 0xe7d34c64a9c85d44u}, /* 1e71 */
    {0xbc8955e946fe31cdu, 0x90e40fbeea1d3a4au}, /* 1e72 */
    {0x6babab6398bdbe41u, 0xb51d13aea4a488ddu}, /* 1e73 */
    {0xc696963c7eed2dd1u, 0xe264589a4dcdab14u}, /* 1e74 */
    {0xfc1e1de5cf543ca2u, 0x8d7eb76070a08aecu}, /* 1e75 */
    {0x3b25a55f43294bcbu, 0xb0de65388cc8ada8u}, /* 1e76 */
    {0x49ef0eb713f39ebeu, 0xdd15fe86affad912u}, /* 1e77 */
    {0x6e3569326c784337u, 0x8a2dbf142dfcc7abu}, /* 1e78 */
    {0x49c2c37f07965404u, 0xacb92ed9397bf996u}, /* 1e79 */
    {0xdc33745ec97be906u, 0xd7e77a8f87daf7fbu}, /* 1e80 */
    {0x69a028bb3ded71a3u, 0x86f0ac99b4e8dafdu}, /* 1e81 */
    {0xc40832ea0d68ce0cu, 0xa8acd7c0222311bcu}, /* 1e82 */
    {0xf50a3fa490c30190u, 0xd2d80db02aabd62bu}, /* 1e83 */
    {0x792667c6da79e0fau, 0x83c7088e1aab65dbu}, /* 1e84 */
    {0x577001b891185938u, 0xa4b8cab1a1563f52u}, /* 1e85 */
    {0xed4c0226b55e6f86u, 0xcde6fd5e09abcf26u}, /* 1e86 */
    {0x544f8158315b05b4u, 0x80b05e5ac60b6178u}, /* 1e87 */
    {0x696361ae3db1c721u, 0xa0dc75f1778e39d6u}, /* 1e88 */
    {0x03bc3a19cd1e38e9u, 0xc913936dd571c84cu}, /* 1e89 */
    {0x04ab48a04065c723u, 0xfb5878494ace3a5fu}, /* 1e90 */
    {0x62eb0d64283f9c76u, 0x9d174b2dcec0e47bu}, /* 1e91 */
    {0x3ba5d0bd324f8394u, 0xc45d1df942711d9au}, /* 1e92 */
    {0xca8f44ec7ee36479u, 0xf5746577930d6500u}, /* 1e93 */
    {0x7e998b13cf4e1ecbu, 0x9968bf6abbe85f20u}, /* 1e94 */
    {0x9e3fedd8c321a67eu, 0xbfc2ef456ae276e8u}, /* 1e95 */
    {0xc5cfe94ef3ea101eu, 0xefb3ab16c59b14a2u}, /* 1e96 */
    {0xbba1f1d158724a12u, 0x95d04aee3b80ece5u}, /* 1e97 */
    {0x2a8a6e45ae8edc97u, 0xbb445da9ca61281fu}, /* 1e98 */
    {0xf52d09d71a3293bdu, 0xea1575143cf97226u}, /* 1e99 */
    {0x593c2626705f9c56u, 0x924d692ca61be758u}, /* 1e100 */
    {0x6f8b2fb00c77836cu, 0xb6e0c377cfa2e12eu}, /* 1e101 */
    {0x0b6dfb9c0f956447u, 0xe498f455c38b997au}, /* 1e102 */
    {0x4724bd4189bd5eacu, 0x8edf98b59a373fecu}, /* 1e103 */
    {0x58edec91ec2cb657u, 0xb2977ee300c50fe7u}, /* 1e104 */
    {0x2f2967b66737e3edu, 0xdf3d5e9bc0f653e1u}, /* 1e105 */
    {0xbd79e0d20082ee74u, 0x8b865b215899f46cu}, /* 1e106 */
    {0xecd8590680a3aa11u, 0xae67f1e9aec07187u}, /* 1e107 */
    {0xe80e6f4820cc9495u, 0xda01ee641a708de9u}, /* 1e108 */
    {0x3109058d147fdcddu, 0x884134fe908658b2u}, /* 1e109 */
    {0xbd4b46f0599fd415u, 0xaa51823e34a7eedeu}, /* 1e110 */
    {0x6c9e18ac7007c91au, 0xd4e5e2cdc1d1ea96u}, /* 1e111 */
    {0x03e2cf6bc604ddb0u, 0x850fadc09923329eu}, /* 1e112 */
    {0x84db8346b786151cu, 0xa6539930bf6bff45u}, /* 1e113 */
    {0xe612641865679a63u, 0xcfe87f7cef46ff16u}, /* 1e114 */
    {0x4fcb7e8f3f60c07eu, 0x81f14fae158c5f6eu}, /* 1e115 */
    {0xe3be5e330f38f09du, 0xa26da3999aef7749u}, /* 1e116 */
    {0x5cadf5bfd3072cc5u, 0xcb090c8001ab551cu}, /* 1e117 */
    {0x73d9732fc7c8f7f6u, 0xfdcb4fa002162a63u}, /* 1e118 */
    {0x2867e7fddcdd9afau, 0x9e9f11c4014dda7eu}, /* 1e119 */
    {0xb281e1fd541501b8u, 0xc646d63501a1511du}, /* 1e120 */
    {0x1f225a7ca91a4226u, 0xf7d88bc24209a565u}, /* 1e121 */
    {0x3375788de9b06958u, 0x9ae757596946075fu}, /* 1e122 */
    {0x0052d6b1641c83aeu, 0xc1a12d2fc3978937u}, /* 1e123 */
    {0xc0678c5dbd23a49au, 0xf209787bb47d6b84u}, /* 1e124 */
    {0xf840b7ba963646e0u, 0x9745eb4d50ce6332u}, /* 1e125 */
    {0xb650e5a93bc3d898u, 0xbd176620a501fbffu}, /* 1e126 */
    {0xa3e51f138ab4cebeu, 0xec5d3fa8ce427affu}, /* 1e127 */
    {0xc66f336c36b10137u, 0x93ba47c980e98cdfu}, /* 1e128 */
    {0xb80b0047445d4184u, 0xb8a8d9bbe123f017u}, /* 1e129 */
    {0xa60dc059157491e5u, 0xe6d3102ad96cec1du}, /* 1e130 */
    {0x87c89837ad68db2fu, 0x9043ea1ac7e41392u}, /* 1e131 */
    {0x29babe4598c311fbu, 0xb454e4a179dd1877u}, /* 1e132 */
    {0xf4296dd6fef3d67au, 0xe16a1dc9d8545e94u}, /* 1e133 */
    {0x1899e4a65f58660cu, 0x8ce2529e2734bb1du}, /* 1e134 */
    {0x5ec05dcff72e7f8fu, 0xb01ae745b101e9e4u}, /* 1e135 */
    {0x76707543f4fa1f73u, 0xdc21a1171d42645du}, /* 1e136 */
    {0x6a06494a791c53a8u, 0x899504ae72497ebau}, /* 1e137 */
    {0x0487db9d17636892u, 0xabfa45da0edbde69u}, /* 1e138 */
    {0x45a9d2845d3c42b6u, 0xd6f8d7509292d603u}, /* 1e139 */
    {0x0b8a2392ba45a9b2u, 0x865b86925b9bc5c2u}, /* 1e140 */
    {0x8e6cac7768d7141eu, 0xa7f26836f282b732u}, /* 1e141 */
    {0x3207d795430cd926u, 0xd1ef0244af2364ffu}, /* 1e142 */
    {0x7f44e6bd49e807b8u, 0x8335616aed761f1fu}, /* 1e143 */
    {0x5f16206c9c6209a6u, 0xa402b9c5a8d3a6e7u}, /* 1e144 */
    {0x36dba887c37a8c0fu, 0xcd036837130890a1u}, /* 1e145 */
    {0xc2494954da2c9789u, 0x802221226be55a64u}, /* 1e146 */
    {0xf2db9baa10b7bd6cu, 0xa02aa96b06deb0fdu}, /* 1e147 */
    {0x6f92829494e5acc7u, 0xc83553c5c8965d3du}, /* 1e148 */
    {0xcb772339ba1f17f9u, 0xfa42a8b73abbf48cu}, /* 1e149 */
    {0xff2a760414536efbu, 0x9c69a97284b578d7u}, /* 1e150 */
    {0xfef5138519684abau, 0xc38413cf25e2d70du}, /* 1e151 */
    {0x7eb258665fc25d69u, 0xf46518c2ef5b8cd1u}, /* 1e152 */
    {0xef2f773ffbd97a61u, 0x98bf2f79d5993802u}, /* 1e153 */
    {0xaafb550ffacfd8fau, 0xbeeefb584aff8603u}, /* 1e154 */
    {0x95ba2a53f983cf38u, 0xeeaaba2e5dbf6784u}, /* 1e155 */
    {0xdd945a747bf26183u, 0x952ab45cfa97a0b2u}, /* 1e156 */
    {0x94f971119aeef9e4u, 0xba756174393d88dfu}, /* 1e157 */
    {0x7a37cd5601aab85du, 0xe912b9d1478ceb17u}, /* 1e158 */
    {0xac62e055c10ab33au, 0x91abb422ccb812eeu}, /* 1e159 */
    {0x577b986b314d6009u, 0xb616a12b7fe617aau}, /* 1e160 */
    {0xed5a7e85fda0b80bu, 0xe39c49765fdf9d94u}, /* 1e161 */
    {0x14588f13be847307u, 0x8e41ade9fbebc27du}, /* 1e162 */
    {0x596eb2d8ae258fc8u, 0xb1d219647ae6b31cu}, /* 1e163 */
    {0x6fca5f8ed9aef3bbu, 0xde469fbd99a05fe3u}, /* 1e164 */
    {0x25de7bb9480d5854u, 0x8aec23d680043beeu}, /* 1e165 */
    {0xaf561aa79a10ae6au, 0xada72ccc20054ae9u}, /* 1e166 */
    {0x1b2ba1518094da04u, 0xd910f7ff28069da4u}, /* 1e167 */
    {0x90fb44d2f05d0842u, 0x87aa9aff79042286u}, /* 1e168 */
    {0x353a1607ac744a53u, 0xa99541bf57452b28u}, /* 1e169 */
    {0x42889b8997915ce8u, 0xd3fa922f2d1675f2u}, /* 1e170 */
    {0x69956135febada11u, 0x847c9b5d7c2e09b7u}, /* 1e171 */
    {0x43fab9837e699095u, 0xa59bc234db398c25u}, /* 1e172 */
    {0x94f967e45e03f4bbu, 0xcf02b2c21207ef2eu}, /* 1e173 */
    {0x1d1be0eebac278f5u, 0x8161afb94b44f57du}, /* 1e174 */
    {0x6462d92a69731732u, 0xa1ba1ba79e1632dcu}, /* 1e175 */
    {0x7d7b8f7503cfdcfeu, 0xca28a291859bbf93u}, /* 1e176 */
    {0x5cda735244c3d43eu, 0xfcb2cb35e702af78u}, /* 1e177 */
    {0x3a0888136afa64a7u, 0x9defbf01b061adabu}, /* 1e178 */
    {0x088aaa1845b8fdd0u, 0xc56baec21c7a1916u}, /* 1e179 */
    {0x8aad549e57273d45u, 0xf6c69a72a3989f5bu}, /* 1e180 */
    {0x36ac54e2f678864bu, 0x9a3c2087a63f6399u}, /* 1e181 */
    {0x84576a1bb416a7ddu, 0xc0cb28a98fcf3c7fu}, /* 1e182 */
    {0x656d44a2a11c51d5u, 0xf0fdf2d3f3c30b9fu}, /* 1e183 */
    {0x9f644ae5a4b1b325u, 0x969eb7c47859e743u}, /* 1e184 */
    {0x873d5d9f0dde1feeu, 0xbc4665b596706114u}, /* 1e185 */
    {0xa90cb506d155a7eau, 0xeb57ff22fc0c7959u}, /* 1e186 */
    {0x09a7f12442d588f2u, 0x9316ff75dd87cbd8u}, /* 1e187 */
    {0x0c11ed6d538aeb2fu, 0xb7dcbf5354e9beceu}, /* 1e188 */
    {0x8f1668c8a86da5fau, 0xe5d3ef282a242e81u}, /* 1e189 */
    {0xf96e017d694487bcu, 0x8fa475791a569d10u}, /* 1e190 */
    {0x37c981dcc395a9acu, 0xb38d92d760ec4455u}, /* 1e191 */
    {0x85bbe253f47b1417u, 0xe070f78d3927556au}, /* 1e192 */
    {0x93956d7478ccec8eu, 0x8c469ab843b89562u}, /* 1e193 */
    {0x387ac8d1970027b2u, 0xaf58416654a6babbu}, /* 1e194 */
    {0x06997b05fcc0319eu, 0xdb2e51bfe9d0696au}, /* 1e195 */
    {0x441fece3bdf81f03u, 0x88fcf317f22241e2u}, /* 1e196 */
    {0xd527e81cad7626c3u, 0xab3c2fddeeaad25au}, /* 1e197 */
    {0x8a71e223d8d3b074u, 0xd60b3bd56a5586f1u}, /* 1e198 */
    {0xf6872d5667844e49u, 0x85c7056562757456u}, /* 1e199 */
    {0xb428f8ac016561dbu, 0xa738c6bebb12d16cu}, /* 1e200 */
    {0xe13336d701beba52u, 0xd106f86e69d785c7u}, /* 1e201 */
    {0xecc0024661173473u, 0x82a45b450226b39cu}, /* 1e202 */
    {0x27f002d7f95d0190u, 0xa34d721642b06084u}, /* 1e203 */
    {0x31ec038df7b441f4u, 0xcc20ce9bd35c78a5u}, /* 1e204 */
    {0x7e67047175a15271u, 0xff290242c83396ceu}, /* 1e205 */
    {0x0f0062c6e984d386u, 0x9f79a169bd203e41u}, /* 1e206 */
    {0x52c07b78a3e60868u, 0xc75809c42c684dd1u}, /* 1e207 */
    {0xa7709a56ccdf8a82u, 0xf92e0c3537826145u}, /* 1e208 */
    {0x88a66076400bb691u, 0x9bbcc7a142b17ccbu}, /* 1e209 */
    {0x6acff893d00ea435u, 0xc2abf989935ddbfeu}, /* 1e210 */
    {0x0583f6b8c4124d43u, 0xf356f7ebf83552feu}, /* 1e211 */
    {0xc3727a337a8b704au, 0x98165af37b2153deu}, /* 1e212 */
    {0x744f18c0592e4c5cu, 0xbe1bf1b059e9a8d6u}, /* 1e213 */
    {0x1162def06f79df73u, 0xeda2ee1c7064130cu}, /* 1e214 */
    {0x8addcb5645ac2ba8u, 0x9485d4d1c63e8be7u}, /* 1e215 */
    {0x6d953e2bd7173692u, 0xb9a74a0637ce2ee1u}, /* 1e216 */
    {0xc8fa8db6ccdd0437u, 0xe8111c87c5c1ba99u}, /* 1e217 */
    {0x1d9c9892400a22a2u, 0x910ab1d4db9914a0u}, /* 1e218 */
    {0x2503beb6d00cab4bu, 0xb54d5e4a127f59c8u}, /* 1e219 */
    {0x2e44ae64840fd61du, 0xe2a0b5dc971f303au}, /* 1e220 */
    {0x5ceaecfed289e5d2u, 0x8da471a9de737e24u}, /* 1e221 */
    {0x7425a83e872c5f47u, 0xb10d8e1456105dadu}, /* 1e222 */
    {0xd12f124e28f77719u, 0xdd50f1996b947518u}, /* 1e223 */
    {0x82bd6b70d99aaa6fu, 0x8a5296ffe33cc92fu}, /* 1e224 */
    {0x636cc64d1001550bu, 0xace73cbfdc0bfb7bu}, /* 1e225 */
    {0x3c47f7e05401aa4eu, 0xd8210befd30efa5au}, /* 1e226 */
    {0x65acfaec34810a71u, 0x8714a775e3e95c78u}, /* 1e227 */
    {0x7f1839a741a14d0du, 0xa8d9d1535ce3b396u}, /* 1e228 */
    {0x1ede48111209a050u, 0xd31045a8341ca07cu}, /* 1e229 */
    {0x934aed0aab460432u, 0x83ea2b892091e44du}, /* 1e230 */
    {0xf81da84d5617853fu, 0xa4e4b66b68b65d60u}, /* 1e231 */
    {0x36251260ab9d668eu, 0xce1de40642e3f4b9u}, /* 1e232 */
    {0xc1d72b7c6b426019u, 0x80d2ae83e9ce78f3u}, /* 1e233 */
    {0xb24cf65b8612f81fu, 0xa1075a24e4421730u}, /* 1e234 */
    {0xdee033f26797b627u, 0xc94930ae1d529cfcu}, /* 1e235 */
    {0x169840ef017da3b1u, 0xfb9b7cd9a4a7443cu}, /* 1e236 */
    {0x8e1f289560ee864eu, 0x9d412e0806e88aa5u}, /* 1e237 */
    {0xf1a6f2bab92a27e2u, 0xc491798a08a2ad4eu}, /* 1e238 */
    {0xae10af696774b1dbu, 0xf5b5d7ec8acb58a2u}, /* 1e239 */
    {0xacca6da1e0a8ef29u, 0x9991a6f3d6bf1765u}, /* 1e240 */
    {0x17fd090a58d32af3u, 0xbff610b0cc6edd3fu}, /* 1e241 */
    {0xddfc4b4cef07f5b0u, 0xeff394dcff8a948eu}, /* 1e242 */
    {0x4abdaf101564f98eu, 0x95f83d0a1fb69cd9u}, /* 1e243 */
    {0x9d6d1ad41abe37f1u, 0xbb764c4ca7a4440fu}, /* 1e244 */
    {0x84c86189216dc5edu, 0xea53df5fd18d5513u}, /* 1e245 */
    {0x32fd3cf5b4e49bb4u, 0x92746b9be2f8552cu}, /* 1e246 */
    {0x3fbc8c33221dc2a1u, 0xb7118682dbb66a77u}, /* 1e247 */
    {0x0fabaf3feaa5334au, 0xe4d5e82392a40515u}, /* 1e248 */
    {0x29cb4d87f2a7400eu, 0x8f05b1163ba6832du}, /* 1e249 */
    {0x743e20e9ef511012u, 0xb2c71d5bca9023f8u}, /* 1e250 */
    {0x914da9246b255416u, 0xdf78e4b2bd342cf6u}, /* 1e251 */
    {0x1ad089b6c2f7548eu, 0x8bab8eefb6409c1au}, /* 1e252 */
    {0xa184ac2473b529b1u, 0xae9672aba3d0c320u}, /* 1e253 */
    {0xc9e5d72d90a2741eu, 0xda3c0f568cc4f3e8u}, /* 1e254 */
    {0x7e2fa67c7a658892u, 0x8865899617fb1871u}, /* 1e255 */
    {0xddbb901b98feeab7u, 0xaa7eebfb9df9de8du}, /* 1e256 */
    {0x552a74227f3ea565u, 0xd51ea6fa85785631u}, /* 1e257 */
    {0xd53a88958f87275fu, 0x8533285c936b35deu}, /* 1e258 */
    {0x8a892abaf368f137u, 0xa67ff273b8460356u}, /* 1e259 */
    {0x2d2b7569b0432d85u, 0xd01fef10a657842cu}, /* 1e260 */
    {0x9c3b29620e29fc73u, 0x8213f56a67f6b29bu}, /* 1e261 */
    {0x8349f3ba91b47b8fu, 0xa298f2c501f45f42u}, /* 1e262 */
    {0x241c70a936219a73u, 0xcb3f2f7642717713u}, /* 1e263 */
    {0xed238cd383aa0110u, 0xfe0efb53d30dd4d7u}, /* 1e264 */
    {0xf4363804324a40aau, 0x9ec95d1463e8a506u}, /* 1e265 */
    {0xb143c6053edcd0d5u, 0xc67bb4597ce2ce48u}, /* 1e266 */
    {0xdd94b7868e94050au, 0xf81aa16fdc1b81dau}, /* 1e267 */
    {0xca7cf2b4191c8326u, 0x9b10a4e5e9913128u}, /* 1e268 */
    {0xfd1c2f611f63a3f0u, 0xc1d4ce1f63f57d72u}, /* 1e269 */
    {0xbc633b39673c8cecu, 0xf24a01a73cf2dccfu}, /* 1e270 */
    {0xd5be0503e085d813u, 0x976e41088617ca01u}, /* 1e271 */
    {0x4b2d8644d8a74e18u, 0xbd49d14aa79dbc82u}, /* 1e272 */
    {0xddf8e7d60ed1219eu, 0xec9c459d51852ba2u}, /* 1e273 */
    {0xcabb90e5c942b503u, 0x93e1ab8252f33b45u}, /* 1e274 */
    {0x3d6a751f3b936243u, 0xb8da1662e7b00a17u}, /* 1e275 */
    {0x0cc512670a783ad4u, 0xe7109bfba19c0c9du}, /* 1e276 */
    {0x27fb2b80668b24c5u, 0x906a617d450187e2u}, /* 1e277 */
    {0xb1f9f660802dedf6u, 0xb484f9dc9641e9dau}, /* 1e278 */
    {0x5e7873f8a0396973u, 0xe1a63853bbd26451u}, /* 1e279 */
    {0xdb0b487b6423e1e8u, 0x8d07e33455637eb2u}, /* 1e280 */
    {0x91ce1a9a3d2cda62u, 0xb049dc016abc5e5fu}, /* 1e281 */
    {0x7641a140cc7810fbu, 0xdc5c5301c56b75f7u}, /* 1e282 */
    {0xa9e904c87fcb0a9du, 0x89b9b3e11b6329bau}, /* 1e283 */
    {0x546345fa9fbdcd44u, 0xac2820d9623bf429u}, /* 1e284 */
    {0xa97c177947ad4095u, 0xd732290fbacaf133u}, /* 1e285 */
    {0x49ed8eabcccc485du, 0x867f59a9d4bed6c0u}, /* 1e286 */
    {0x5c68f256bfff5a74u, 0xa81f301449ee8c70u}, /* 1e287 */
    {0x73832eec6fff3111u, 0xd226fc195c6a2f8cu}, /* 1e288 */
    {0xc831fd53c5ff7eabu, 0x83585d8fd9c25db7u}, /* 1e289 */
    {0xba3e7ca8b77f5e55u, 0xa42e74f3d032f525u}, /* 1e290 */
    {0x28ce1bd2e55f35ebu, 0xcd3a1230c43fb26fu}, /* 1e291 */
    {0x7980d163cf5b81b3u, 0x80444b5e7aa7cf85u}, /* 1e292 */
    {0xd7e105bcc332621fu, 0xa0555e361951c366u}, /* 1e293 */
    {0x8dd9472bf3fefaa7u, 0xc86ab5c39fa63440u}, /* 1e294 */
    {0xb14f98f6f0feb951u, 0xfa856334878fc150u}, /* 1e295 */
    {0x6ed1bf9a569f33d3u, 0x9c935e00d4b9d8d2u}, /* 1e296 */
    {0x0a862f80ec4700c8u, 0xc3b8358109e84f07u}, /* 1e297 */
    {0xcd27bb612758c0fau, 0xf4a642e14c6262c8u}, /* 1e298 */
    {0x8038d51cb897789cu, 0x98e7e9cccfbd7dbdu}, /* 1e299 */
    {0xe0470a63e6bd56c3u, 0xbf21e44003acdd2cu}, /* 1e300 */
    {0x1858ccfce06cac74u, 0xeeea5d5004981478u}, /* 1e301 */
    {0x0f37801e0c43ebc8u, 0x95527a5202df0ccbu}, /* 1e302 */
    {0xd30560258f54e6bau, 0xbaa718e68396cffdu}, /* 1e303 */
    {0x47c6b82ef32a2069u, 0xe950df20247c83fdu}, /* 1e304 */
    {0x4cdc331d57fa5441u, 0x91d28b7416cdd27eu}, /* 1e305 */
    {0xe0133fe4adf8e952u, 0xb6472e511c81471du}, /* 1e306 */
    {0x58180fddd97723a6u, 0xe3d8f9e563a198e5u}, /* 1e307 */
    {0x570f09eaa7ea7648u, 0x8e679c2f5e44ff8fu}, /* 1e308 */
    {0x2cd2cc6551e513dau, 0xb201833b35d63f73u}, /* 1e309 */
    {0xf8077f7ea65e58d1u, 0xde81e40a034bcf4fu}, /* 1e310 */
    {0xfb04afaf27faf782u, 0x8b112e86420f6191u}, /* 1e311 */
    {0x79c5db9af1f9b563u, 0xadd57a27d29339f6u}, /* 1e312 */
    {0x18375281ae7822bcu, 0xd94ad8b1c7380874u}, /* 1e313 */
    {0x8f2293910d0b15b5u, 0x87cec76f1c830548u}, /* 1e314 */
    {0xb2eb3875504ddb22u, 0xa9c2794ae3a3c69au}, /* 1e315 */
    {0x5fa60692a46151ebu, 0xd433179d9c8cb841u}, /* 1e316 */
    {0xdbc7c41ba6bcd333u, 0x849feec281d7f328u}, /* 1e317 */
    {0x12b9b522906c0800u, 0xa5c7ea73224deff3u}, /* 1e318 */
    {0xd768226b34870a00u, 0xcf39e50feae16befu}, /* 1e319 */
    {0xe6a1158300d46640u, 0x81842f29f2cce375u}, /* 1e320 */
    {0x60495ae3c1097fd0u, 0xa1e53af46f801c53u}, /* 1e321 */
    {0x385bb19cb14bdfc4u, 0xca5e89b18b602368u}, /* 1e322 */
    {0x46729e03dd9ed7b5u, 0xfcf62c1dee382c42u}, /* 1e323 */
    {0x6c07a2c26a8346d1u, 0x9e19db92b4e31ba9u}, /* 1e324 */
    {0xc7098b7305241885u, 0xc5a05277621be293u}, /* 1e325 */
    {0xb8cbee4fc66d1ea7u, 0xf70867153aa2db38u}, /* 1e326 */
    {0x737f74f1dc043328u, 0x9a65406d44a5c903u}, /* 1e327 */
    {0x505f522e53053ff2u, 0xc0fe908895cf3b44u}, /* 1e328 */
    {0x647726b9e7c68fefu, 0xf13e34aabb430a15u}, /* 1e329 */
    {0x5eca783430dc19f5u, 0x96c6e0eab509e64du}, /* 1e330 */
    {0xb67d16413d132072u, 0xbc789925624c5fe0u}, /* 1e331 */
    {0xe41c5bd18c57e88fu, 0xeb96bf6ebadf77d8u}, /* 1e332 */
    {0x8e91b962f7b6f159u, 0x933e37a534cbaae7u}, /* 1e333 */
    {0x723627bbb5a4adb0u, 0xb80dc58e81fe95a1u}, /* 1e334 */
    {0xcec3b1aaa30dd91cu, 0xe61136f2227e3b09u}, /* 1e335 */
    {0x213a4f0aa5e8a7b1u, 0x8fcac257558ee4e6u}, /* 1e336 */
    {0xa988e2cd4f62d19du, 0xb3bd72ed2af29e1fu}, /* 1e337 */
    {0x93eb1b80a33b8605u, 0xe0accfa875af45a7u}, /* 1e338 */
    {0xbc72f130660533c3u, 0x8c6c01c9498d8b88u}, /* 1e339 */
    {0xeb8fad7c7f8680b4u, 0xaf87023b9bf0ee6au}, /* 1e340 */
    {0xa67398db9f6820e1u, 0xdb68c2ca82ed2a05u}, /* 1e341 */
    {0x88083f8943a1148cu, 0x892179be91d43a43u}, /* 1e342 */
    {0x6a0a4f6b948959b0u, 0xab69d82e364948d4u}, /* 1e343 */
    {0x848ce34679abb01cu, 0xd6444e39c3db9b09u}, /* 1e344 */
    {0xf2d80e0c0c0b4e11u, 0x85eab0e41a6940e5u}, /* 1e345 */
    {0x6f8e118f0f0e2195u, 0xa7655d1d2103911fu}, /* 1e346 */
    {0x4b7195f2d2d1a9fbu, 0xd13eb46469447567u}, /* 1e347 */
};

static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static int clz64(uint64_t x) {
    int n = 0;
    if (!(x >> 32)) n += 32, x <<= 32;
    if (!(x >> 48)) n += 16, x <<= 16;
    if (!(x >> 56)) n += 8, x <<= 8;
    if (!(x >> 60)) n += 4, x <<= 4;
    if (!(x >> 62)) n += 2, x <<= 2;
    if (!(x >> 63)) n += 1;
    return n;
}

static double from_bits(uint64_t b) {
    double x;
    memcpy(&x, &b, 8);
    return x;
}

/* Go's eiselLemire64. man != 0. 1 and *out when decided, 0 when the slow path must answer. */
static int eisel_lemire(uint64_t man, int64_t e10, int neg, double *out) {
    if (e10 < P10_MIN || e10 > P10_MAX) return 0;
    int clz = clz64(man);
    man <<= clz;
    uint64_t exp2 = (uint64_t)(((217706 * e10) >> 16) + 64 + 1023) - (uint64_t)clz;
    const uint64_t *p = P10[e10 - P10_MIN];
    uint64_t xhi, xlo;
    mul64(man, p[1], &xhi, &xlo);
    if ((xhi & 0x1FF) == 0x1FF && xlo + man < man) {
        uint64_t yhi, ylo;
        mul64(man, p[0], &yhi, &ylo);
        uint64_t mhi = xhi, mlo = xlo + yhi;
        if (mlo < xlo) mhi++;
        if ((mhi & 0x1FF) == 0x1FF && mlo + 1 == 0 && ylo + man < man) return 0;
        xhi = mhi, xlo = mlo;
    }
    uint64_t msb = xhi >> 63;
    uint64_t mant = xhi >> (msb + 9);
    exp2 -= 1 ^ msb;
    if (xlo == 0 && (xhi & 0x1FF) == 0 && (mant & 3) == 1) return 0;
    mant += mant & 1;
    mant >>= 1;
    if (mant >> 53) mant >>= 1, exp2 += 1;
    if (exp2 - 1 >= 0x7FF - 1) return 0;
    uint64_t bits = exp2 << 52 | (mant & 0x000FFFFFFFFFFFFFull);
    if (neg) bits |= 0x8000000000000000ull;
    *out = from_bits(bits);
    return 1;
}

/* ---- the slow path: Go's decimal ---- */
#define DEC_MAX 800
typedef struct {
    uint8_t d[DEC_MAX]; /* digit values 0..9, no leading zero */
    int nd;             /* digits used */
    int64_t dp;         /* decimal point: value = 0.d[0..nd) * 10^dp */
    int trunc;          /* nonzero digits were dropped */
} Dec;

static void dec_trim(Dec *a) {
    while (a->nd > 0 && a->d[a->nd - 1] == 0) a->nd--;
    if (a->nd == 0) a->dp = 0;
}

/* a * 2^k, k in 1..60 */
static void dec_lshift(Dec *a, int k) {
    uint8_t t[DEC_MAX + 24];
    int w = (int)sizeof t;
    uint64_t n = 0;
    for (int r = a->nd - 1; r >= 0; r--) {
        n += (uint64_t)a->d[r] << k;
        uint64_t q = n / 10;
        t[--w] = (uint8_t)(n - 10 * q);
        n = q;
    }
    while (n > 0) {
        uint64_t q = n / 10;
        t[--w] = (uint8_t)(n - 10 * q);
        n = q;
    }
    int nd = (int)sizeof t - w;
    a->dp += nd - a->nd;
    if (nd > DEC_MAX) {
        for (int i = DEC_MAX; i < nd; i++)
            if (t[w + i]) a->trunc = 1;
        nd = DEC_MAX;
    }
    memcpy(a->d, t + w, (size_t)nd);
    a->nd = nd;
    dec_trim(a);
}

/* a / 2^k, k in 1..60 */
static void dec_rshift(Dec *a, int k) {
    int r = 0, w = 0;
    uint64_t n = 0;
    for (; (n >> k) == 0; r++) {
        if (r >= a->nd) {
            if (n == 0) {
                a->nd = 0;
                a->dp = 0;
                return;
            }
            while ((n >> k) == 0) n *= 10, r++;
            break;
        }
        n = n * 10 + a->d[r];
    }
    a->dp -= r - 1;
    uint64_t mask = ((uint64_t)1 << k) - 1;
    for (; r < a->nd; r++) {
        uint64_t c = a->d[r], dig = n >> k;
        n &= mask;
        a->d[w++] = (uint8_t)dig;
        n = n * 10 + c;
    }
    while (n > 0) {
        uint64_t dig = n >> k;
        n &= mask;
        if (w < DEC_MAX) a->d[w++] = (uint8_t)dig;
        else if (dig > 0) a->trunc = 1;
        n *= 10;
    }
    a->nd = w;
    dec_trim(a);
}

static void dec_shift(Dec *a, int k) {
    if (a->nd == 0) return;
    for (; k > 60; k -= 60) dec_lshift(a, 60);
    for (; k < -60; k += 60) dec_rshift(a, 60);
    if (k > 0) dec_lshift(a, k);
    else if (k < 0) dec_rshift(a, -k);
}

static int dec_round_up(const Dec *a, int64_t nd) {
    if (nd < 0 || nd >= a->nd) return 0;
    if (a->d[nd] == 5 && nd + 1 == a->nd) { /* exactly halfway: to even, unless digits were dropped */
        if (a->trunc) return 1;
        return nd > 0 && a->d[nd - 1] % 2;
    }
    return a->d[nd] >= 5;
}

static uint64_t dec_rounded_int(const Dec *a) {
    if (a->dp > 20) return UINT64_MAX;
    int64_t i = 0;
    uint64_t n = 0;
    for (; i < a->dp && i < a->nd; i++) n = n * 10 + a->d[i];
    for (; i < a->dp; i++) n *= 10;
    if (dec_round_up(a, a->dp)) n++;
    return n;
}

static double dec_to_double(Dec *d, int neg) {
    static const int powtab[] = {1, 3, 6, 9, 13, 16, 19, 23, 26};
    const int bias = -1023;
    int64_t exp = 0;
    uint64_t mant = 0;
    uint64_t bits;
    if (d->nd == 0 || d->dp < -330) {
        exp = bias;
        goto out;
    }
    if (d->dp > 310) goto overflow;
    while (d->dp > 0) {
        int n = d->dp >= 9 ? 27 : powtab[d->dp];
        dec_shift(d, -n);
        exp += n;
    }
    while (d->dp < 0 || (d->dp == 0 && d->d[0] < 5)) {
        int n = -d->dp >= 9 ? 27 : powtab[-d->dp];
        dec_shift(d, n);
        exp -= n;
    }
    exp--; /* [0.5, 1) -> [1, 2) */
    if (exp < bias + 1) {
        int n = (int)(bias + 1 - exp);
        dec_shift(d, -n);
        exp += n;
    }
    if (exp - bias >= 0x7FF) goto overflow;
    dec_shift(d, 53);
    mant = dec_rounded_int(d);
    if (mant == (uint64_t)2 << 52) {
        mant >>= 1;
        exp++;
        if (exp - bias >= 0x7FF) goto overflow;
    }
    if (!(mant & ((uint64_t)1 << 52))) exp = bias;
    goto out;
overflow:
    mant = 0;
    exp = 0x7FF + bias;
out:
    bits = mant & (((uint64_t)1 << 52) - 1);
    bits |= (uint64_t)((exp - bias) & 0x7FF) << 52;
    if (neg) bits |= 0x8000000000000000ull;
    return from_bits(bits);
}

double cc_strtod(const char *s, size_t n) {
    size_t i = 0;
    int neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i] == '-', i++;
    /* Significant digits: the first 19 in `man`, the count of all in `nsig`; `dp` = position of
     * the decimal point relative to the first significant digit. */
    uint64_t man = 0;
    int64_t nsig = 0, dp = 0;
    int seen_point = 0;
    size_t start = i;
    for (; i < n; i++) {
        char c = s[i];
        if (c == '.' && !seen_point) {
            seen_point = 1;
            continue;
        }
        if (c < '0' || c > '9') break;
        if (c == '0' && nsig == 0) {
            if (seen_point) dp--;
            continue;
        }
        if (nsig < 19) man = man * 10 + (uint64_t)(c - '0');
        nsig++;
        if (!seen_point) dp++;
    }
    size_t end_mant = i;
    int64_t e = 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        int eneg = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i] == '-', i++;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
            if (e < 100000000) e = e * 10 + (s[i] - '0');
        if (eneg) e = -e;
    }
    if (nsig == 0) return neg ? -0.0 : 0.0;
    double x;
    /* value = 0.digits * 10^(dp + e) = man * 10^(dp + e - min(nsig, 19)) (+ dropped digits) */
    int64_t e10 = dp + e - (nsig < 19 ? nsig : 19);
    if (nsig <= 19) {
        if (eisel_lemire(man, e10, neg, &x)) return x;
    } else {
        double y;
        if (eisel_lemire(man, e10, neg, &x) && eisel_lemire(man + 1, e10, neg, &y) && x == y) return x;
    }
    /* Slow path: all the digits again. */
    Dec d;
    d.nd = 0, d.trunc = 0;
    d.dp = dp + e;
    for (size_t j = start; j < end_mant; j++) {
        char c = s[j];
        if (c < '0' || c > '9') continue;
        if (c == '0' && d.nd == 0) continue;
        if (d.nd < DEC_MAX) d.d[d.nd++] = (uint8_t)(c - '0');
        else if (c != '0') d.trunc = 1;
    }
    dec_trim(&d);
    if (d.nd == 0) return neg ? -0.0 : 0.0;
    return dec_to_double(&d, neg);
}
