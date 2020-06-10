// 水仙花数的定义是，这个数等于他每一位数上的幂次之和 见维基百科的定义

/*
    初始化一个0~9的n次方的数组，
    依次遍历，对每一个数 依次循环做 /10 ， 每次之后将数据的n次方取出相加
    判断结果一致即可！
*/

#include <iostream>
using namespace std;

class Solution {
public:
    /**
     * @param n: The number of digits
     * @return: All narcissistic numbers with n digits
     */
    vector<int> getNarcissisticNumbers(int n) {
        vector<int> results;
        if (n == 0)
            return results;

        vector<int> powers(10, 0);
        for (int i = 1; i < 10; ++i) {
            powers[i] = pow(i, n);          //0~9的n次方
        }

        int upperLimit = pow(10, n) - 1;    //上限
        int lowerLimit = (n == 1) ? 0 : pow(10, n - 1); //下限

        for (int num = lowerLimit; num <= upperLimit; ++num) {
            vector<int> digits(n, 0);
            long long powSum = 0;
            int saveNum = num;
            for (int i = 0; i < n; ++i) {
                digits[i] = saveNum % 10;
                powSum += powers[digits[i]];
                saveNum /= 10;
            }

            if (num == powSum)
                results.push_back(num);
        }

        return results;
    }
};
