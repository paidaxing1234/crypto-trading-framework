/**
 * 本地存储工具
 */

const STORAGE_PREFIX = 'trading_platform_'

class Storage {
  constructor(key) {
    this.key = STORAGE_PREFIX + key
  }

  load(defaultValue = null) {
    try {
      const data = localStorage.getItem(this.key)
      return data ? JSON.parse(data) : defaultValue
    } catch (error) {
      console.error(`Load ${this.key} error:`, error)
      return defaultValue
    }
  }

  save(data) {
    try {
      localStorage.setItem(this.key, JSON.stringify(data))
      return true
    } catch (error) {
      console.error(`Save ${this.key} error:`, error)
      return false
    }
  }

  remove() {
    try {
      localStorage.removeItem(this.key)
      return true
    } catch (error) {
      console.error(`Remove ${this.key} error:`, error)
      return false
    }
  }
}

export const strategyStorage = new Storage('strategies')
export const accountStorage = new Storage('accounts')
export const orderStorage = new Storage('orders')
export const positionStorage = new Storage('positions')
export const userStorage = new Storage('user')
