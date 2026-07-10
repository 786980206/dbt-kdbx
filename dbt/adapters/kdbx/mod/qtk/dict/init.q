
// @kind function
// @subcategory dict
// @overview Return the key of a dictionary. It's an alias of [key](https://code.kx.com/q/ref/key/#key-of-a-dictionary).
// @param dict {dict} A dictionary.
// @return {any[]} The key of a dictionary.
// @doctest
// .qtk:use`qtk;
//
// `a`b~.qtk.dict.key `a`b!1 2
.z.m.key:{[dict] key dict};

export:([.z.m.key]);
